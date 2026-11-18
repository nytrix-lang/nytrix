/*
 * Exact i64 diamond if-conversion.
 *
 * The pass only speculates a whitelist of non-trapping scalar operations.  It
 * requires header -> {false,true} -> join with two exclusive predecessors and
 * exactly one join PHI, then moves both bounded arms before SELECT_I64.
 */
#include "code/backend/passes/util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  size_t insts[8];
  size_t count;
  size_t branch;
  int value;
  int64_t label;
} ifc_arm_t;

typedef struct {
  int64_t slot;
  int false_value;
  int true_value;
} ifc_store_pair_t;

typedef struct {
  size_t phi_index;
  int false_value;
  int true_value;
} ifc_phi_pair_t;

static size_t ifc_label_block(const nyr_cfg_t *cfg, int64_t label) {
  if (!cfg)
    return SIZE_MAX;
  for (size_t block = 0; block < cfg->block_count; ++block)
    if (cfg->block_label[block] == label)
      return block;
  return SIZE_MAX;
}

static bool ifc_single_predecessor(const nyr_cfg_t *cfg, size_t block,
                                   size_t predecessor) {
  return cfg && block < cfg->block_count &&
         cfg->pred_offsets[block + 1] - cfg->pred_offsets[block] == 1 &&
         cfg->pred_blocks[cfg->pred_offsets[block]] == predecessor;
}

static bool ifc_two_predecessors(const nyr_cfg_t *cfg, size_t block,
                                 size_t first, size_t second) {
  if (!cfg || block >= cfg->block_count ||
      cfg->pred_offsets[block + 1] - cfg->pred_offsets[block] != 2)
    return false;
  size_t begin = cfg->pred_offsets[block];
  size_t left = cfg->pred_blocks[begin];
  size_t right = cfg->pred_blocks[begin + 1];
  return (left == first && right == second) ||
         (left == second && right == first);
}

static bool ifc_diamond_is_in_loop(const nyr_cfg_t *cfg, size_t header,
                                   size_t false_block, size_t true_block,
                                   size_t join) {
  if (!cfg || !cfg->block_count)
    return false;
  bool *members = calloc(cfg->block_count, sizeof(*members));
  if (!members)
    return false;
  bool in_loop = false;
  for (size_t latch = 0; latch < cfg->block_count && !in_loop; ++latch) {
    for (size_t edge = cfg->succ_offsets[latch];
         edge < cfg->succ_offsets[latch + 1]; ++edge) {
      size_t loop_header = cfg->succ_blocks[edge];
      if (!nyr_cfg_is_backedge(cfg, latch, loop_header) ||
          !nyr_cfg_natural_loop_blocks(cfg, latch, loop_header, members,
                                       cfg->block_count))
        continue;
      in_loop = members[header] && members[false_block] &&
                members[true_block] && members[join];
      if (in_loop)
        break;
    }
  }
  free(members);
  return in_loop;
}

static size_t ifc_arm_cost(const nyr_func_t *f, const ifc_arm_t *arm) {
  size_t cost = 0;
  for (size_t i = 0; i < arm->count; ++i) {
    nyr_op_t op = f->data[arm->insts[i]].op;
    if (op == NYIR_MUL_I64)
      cost += 4;
    else if (op != NYIR_CONST_I64 && op != NYIR_COPY)
      ++cost;
  }
  return cost;
}

/*
 * Source lowering can leave empty forward blocks on either diamond edge,
 * including an unlabeled fallthrough block after BR_IF. The exclusive-edge
 * check below still requires a unique predecessor through every bridge.
 */
static size_t ifc_resolve_empty_bridges(const nyr_func_t *f,
                                        const nyr_cfg_t *cfg, size_t block) {
  for (size_t hops = 0;
       f && cfg && block < cfg->block_count && hops < cfg->block_count;
       ++hops) {
    if (cfg->succ_offsets[block + 1] - cfg->succ_offsets[block] != 1)
      return block;
    size_t branch = SIZE_MAX;
    bool empty = true;
    for (size_t i = cfg->block_start[block]; i < cfg->block_end[block]; ++i) {
      nyr_op_t op = f->data[i].op;
      if (op == NYIR_LABEL || op == NYIR_NOP)
        continue;
      if (op == NYIR_BR && branch == SIZE_MAX)
        branch = i;
      else
        empty = false;
    }
    if (!empty || branch == SIZE_MAX)
      return block;
    block = cfg->succ_blocks[cfg->succ_offsets[block]];
  }
  return block;
}

static bool ifc_exclusive_edge(const nyr_func_t *f, const nyr_cfg_t *cfg,
                               size_t header, size_t edge_entry,
                               size_t resolved) {
  size_t predecessor = header;
  for (size_t hops = 0;
       hops < cfg->block_count && edge_entry < cfg->block_count; ++hops) {
    if (!ifc_single_predecessor(cfg, edge_entry, predecessor))
      return false;
    if (edge_entry == resolved)
      return true;
    size_t branch = SIZE_MAX;
    for (size_t i = cfg->block_start[edge_entry];
         i < cfg->block_end[edge_entry]; ++i) {
      nyr_op_t op = f->data[i].op;
      if (op == NYIR_LABEL || op == NYIR_NOP)
        continue;
      if (op != NYIR_BR || branch != SIZE_MAX)
        return false;
      branch = i;
    }
    if (branch == SIZE_MAX ||
        cfg->succ_offsets[edge_entry + 1] - cfg->succ_offsets[edge_entry] != 1)
      return false;
    predecessor = edge_entry;
    edge_entry = cfg->succ_blocks[cfg->succ_offsets[edge_entry]];
  }
  return false;
}

/*
 * A resolved arm may be the join itself when the arm block only branches to
 * it.  That edge is still exclusive: every block on the way down to the join
 * has exactly one predecessor, and the join owns the diamond's two.
 */
static bool ifc_exclusive_bridge(const nyr_func_t *f, const nyr_cfg_t *cfg,
                                 size_t header, size_t entry, size_t join,
                                 size_t *arm_block) {
  size_t predecessor = header;
  size_t last = SIZE_MAX;
  for (size_t hops = 0;
       hops < cfg->block_count && entry < cfg->block_count; ++hops) {
    if (entry == join) {
      if (arm_block)
        *arm_block = last;
      return last != SIZE_MAX;
    }
    if (!ifc_single_predecessor(cfg, entry, predecessor))
      return false;
    last = entry;
    size_t branch = SIZE_MAX;
    for (size_t i = cfg->block_start[entry]; i < cfg->block_end[entry]; ++i) {
      nyr_op_t op = f->data[i].op;
      if (op == NYIR_LABEL || op == NYIR_NOP)
        continue;
      if (op != NYIR_BR || branch != SIZE_MAX)
        return false;
      branch = i;
    }
    if (branch == SIZE_MAX ||
        cfg->succ_offsets[entry + 1] - cfg->succ_offsets[entry] != 1)
      return false;
    predecessor = entry;
    entry = cfg->succ_blocks[cfg->succ_offsets[entry]];
  }
  return false;
}

static bool ifc_boolean_condition(const nyr_func_t *f, const int *defs,
                                  int value, unsigned depth) {
  if (!f || !defs || value < 0 || value >= f->next_value || depth > 16 ||
      defs[value] < 0)
    return false;
  const nyr_inst_t *def = &f->data[(size_t)defs[value]];
  if (def->op == NYIR_CMP_I64 || def->op == NYIR_CMP_F64 ||
      def->op == NYIR_CMP_F32)
    return true;
  if (def->op == NYIR_CONST_I64)
    return def->imm == 0 || def->imm == 1;
  return def->op == NYIR_COPY &&
         ifc_boolean_condition(f, defs, def->a, depth + 1);
}

static bool ifc_safe_arm_op(const nyr_inst_t *in) {
  bool local_load = in && in->op == NYIR_LOAD_LOCAL &&
                    nyr_inst_effects(in) == NYIR_EFFECT_READ_LOCAL &&
                    in->scalar_class == NYIR_SCALAR_I64_RAW;
  bool local_store = in && in->op == NYIR_STORE_LOCAL &&
                     nyr_inst_effects(in) == NYIR_EFFECT_WRITE_LOCAL &&
                     in->scalar_class == NYIR_SCALAR_I64_RAW;
  if (!in || (in->dst < 0 && !local_store) ||
      (!local_load && !local_store &&
       nyr_inst_effects(in) != NYIR_EFFECT_NONE) ||
      in->extra_args || in->extra_args_len || in->arg_sizes ||
      in->phi_incoming || in->phi_incoming_len || in->symbol)
    return false;
  switch (in->op) {
  case NYIR_LOAD_LOCAL:
    return local_load && in->imm >= 0;
  case NYIR_STORE_LOCAL:
    return local_store && in->imm >= 0 && in->a >= 0;
  case NYIR_CONST_I64:
  case NYIR_COPY:
  case NYIR_ADD_I64:
  case NYIR_SUB_I64:
  case NYIR_MUL_I64:
  case NYIR_AND_I64:
  case NYIR_OR_I64:
  case NYIR_XOR_I64:
  case NYIR_SHL_I64:
  case NYIR_SAR_I64:
  case NYIR_ROR_I64:
  case NYIR_ROR32_I64:
  case NYIR_BSWAP_I64:
  case NYIR_BSWAP32_I64:
  case NYIR_CMP_I64:
    return true;
  default:
    return false;
  }
}

static bool ifc_value_available(const nyr_cfg_t *cfg, const int *defs,
                                const ifc_arm_t *arm, size_t header,
                                size_t header_branch, int value) {
  if (!cfg || !defs || !arm || value < 0 || defs[value] < 0)
    return false;
  size_t def = (size_t)defs[value];
  for (size_t i = 0; i < arm->count; ++i)
    if (arm->insts[i] == def)
      return true;
  size_t def_block = cfg->inst_block[def];
  return (def_block == header && def < header_branch) ||
         (def_block != header && nyr_cfg_dominates(cfg, def_block, header));
}

static bool ifc_arm_operands_available(const nyr_cfg_t *cfg, const int *defs,
                                       const ifc_arm_t *arm, size_t header,
                                       size_t header_branch,
                                       const nyr_inst_t *in) {
  if (in->op == NYIR_CONST_I64 || in->op == NYIR_LOAD_LOCAL)
    return true;
  if (!ifc_value_available(cfg, defs, arm, header, header_branch, in->a))
    return false;
  if (in->op == NYIR_COPY || in->op == NYIR_STORE_LOCAL)
    return true;
  return ifc_value_available(cfg, defs, arm, header, header_branch, in->b);
}

static bool ifc_local_load_is_initialized(const nyr_func_t *f,
                                          const nyr_cfg_t *cfg, size_t header,
                                          size_t header_branch, int64_t slot) {
  if (!f || !cfg || slot < 0)
    return false;
  bool initialized = (uint64_t)slot < f->param_count;
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->op == NYIR_ADDR_LOCAL && in->imm == slot)
      return false;
    if (in->op != NYIR_STORE_LOCAL || in->imm != slot)
      continue;
    size_t block = cfg->inst_block[i];
    if ((block == header && i < header_branch) ||
        (block != header && nyr_cfg_dominates(cfg, block, header)))
      initialized = true;
  }
  return initialized;
}

static bool ifc_arm_store_pairs(const nyr_func_t *f, const ifc_arm_t *false_arm,
                                const ifc_arm_t *true_arm,
                                ifc_store_pair_t *pairs, size_t *pair_count) {
  if (!f || !false_arm || !true_arm || !pairs || !pair_count)
    return false;
  *pair_count = 0;
  bool matched[8] = {false};
  for (size_t i = 0; i < false_arm->count; ++i) {
    const nyr_inst_t *false_store = &f->data[false_arm->insts[i]];
    if (false_store->op != NYIR_STORE_LOCAL)
      continue;
    if (*pair_count >= 8)
      return false;
    for (size_t at = 0; at < f->len; ++at)
      if (f->data[at].op == NYIR_ADDR_LOCAL &&
          f->data[at].imm == false_store->imm)
        return false;
    size_t match = SIZE_MAX;
    for (size_t j = 0; j < true_arm->count; ++j) {
      const nyr_inst_t *true_store = &f->data[true_arm->insts[j]];
      if (true_store->op != NYIR_STORE_LOCAL ||
          true_store->imm != false_store->imm)
        continue;
      if (match != SIZE_MAX)
        return false;
      match = j;
    }
    if (match == SIZE_MAX || matched[match])
      return false;
    const nyr_inst_t *true_store = &f->data[true_arm->insts[match]];
    matched[match] = true;
    pairs[(*pair_count)++] = (ifc_store_pair_t){
        .slot = false_store->imm,
        .false_value = false_store->a,
        .true_value = true_store->a,
    };
  }
  for (size_t j = 0; j < true_arm->count; ++j)
    if (f->data[true_arm->insts[j]].op == NYIR_STORE_LOCAL && !matched[j])
      return false;
  return true;
}

static bool ifc_arms_have_stable_local_loads(const nyr_func_t *f,
                                             const nyr_cfg_t *cfg,
                                             size_t header,
                                             size_t header_branch,
                                             const ifc_arm_t *false_arm,
                                             const ifc_arm_t *true_arm) {
  const ifc_arm_t *arms[] = {false_arm, true_arm};
  for (size_t a = 0; a < 2; ++a) {
    const ifc_arm_t *arm = arms[a];
    for (size_t i = 0; i < arm->count; ++i) {
      const nyr_inst_t *load = &f->data[arm->insts[i]];
      if (load->op != NYIR_LOAD_LOCAL)
        continue;
      if (!ifc_local_load_is_initialized(f, cfg, header, header_branch,
                                         load->imm))
        return false;
      for (size_t other_arm = 0; other_arm < 2; ++other_arm) {
        const ifc_arm_t *writes = arms[other_arm];
        for (size_t j = 0; j < writes->count; ++j) {
          const nyr_inst_t *write = &f->data[writes->insts[j]];
          if (write->op == NYIR_STORE_LOCAL && write->imm == load->imm &&
              other_arm == a && writes->insts[j] < arm->insts[i])
            return false;
        }
      }
    }
  }
  return true;
}

static bool ifc_parse_arm(const nyr_func_t *f, const nyr_cfg_t *cfg,
                          const int *defs, size_t block, size_t header,
                          size_t header_branch, int64_t join_label,
                          ifc_arm_t *arm) {
  if (!f || !cfg || !defs || !arm || block >= cfg->block_count ||
      cfg->block_label[block] < 0 ||
      cfg->succ_offsets[block + 1] - cfg->succ_offsets[block] != 1 ||
      cfg->block_label[cfg->succ_blocks[cfg->succ_offsets[block]]] !=
          join_label)
    return false;
  *arm = (ifc_arm_t){
      .branch = SIZE_MAX, .value = -1, .label = cfg->block_label[block]};
  for (size_t i = cfg->block_start[block]; i < cfg->block_end[block]; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->op == NYIR_LABEL || in->op == NYIR_NOP)
      continue;
    if (in->op == NYIR_BR) {
      if (in->imm != join_label || arm->branch != SIZE_MAX)
        return false;
      arm->branch = i;
      continue;
    }
    if (arm->branch != SIZE_MAX || arm->count == 8 || !ifc_safe_arm_op(in) ||
        !ifc_arm_operands_available(cfg, defs, arm, header, header_branch, in))
      return false;
    arm->insts[arm->count++] = i;
  }
  /*
   * An arm that only branches to the join contributes no speculation: its
   * value is the one the PHI already names for this edge.
   */
  return arm->count != 0 || arm->branch != SIZE_MAX;
}

static bool ifc_arm_defines(const nyr_func_t *f, const ifc_arm_t *arm,
                            int value);

static bool ifc_arm_value_ok(const nyr_func_t *f, const nyr_cfg_t *cfg,
                             const int *defs, const ifc_arm_t *arm,
                             size_t header, size_t header_branch, int value) {
  if (ifc_arm_defines(f, arm, value))
    return true;
  return ifc_value_available(cfg, defs, arm, header, header_branch, value);
}

/*
 * A join may merge several values; convert them all so the arm blocks can be
 * removed without leaving PHIs that name dead predecessors.
 */
static bool ifc_join_phis(const nyr_func_t *f, const nyr_cfg_t *cfg, size_t join,
                          const ifc_arm_t *false_arm,
                          const ifc_arm_t *true_arm, ifc_phi_pair_t *pairs,
                          size_t *pair_count) {
  if (!f || !cfg || !false_arm || !true_arm || !pairs || !pair_count ||
      join >= cfg->block_count || cfg->block_label[join] < 0)
    return false;
  *pair_count = 0;
  for (size_t i = cfg->block_start[join]; i < cfg->block_end[join]; ++i) {
    const nyr_inst_t *phi = &f->data[i];
    if (phi->op != NYIR_PHI)
      continue;
    if (*pair_count >= 8)
      return false;
    if (phi->dst < 0 || phi->phi_incoming_len != 2 || !phi->phi_incoming)
      return false;
    int false_value = -1, true_value = -1;
    for (size_t k = 0; k < phi->phi_incoming_len; ++k) {
      if (phi->phi_incoming[k].predecessor_label == false_arm->label)
        false_value = phi->phi_incoming[k].value;
      else if (phi->phi_incoming[k].predecessor_label == true_arm->label)
        true_value = phi->phi_incoming[k].value;
      else
        return false;
    }
    if (false_value < 0 || true_value < 0)
      return false;
    pairs[(*pair_count)++] = (ifc_phi_pair_t){i, false_value, true_value};
  }
  return *pair_count > 0;
}

static bool ifc_arm_defines(const nyr_func_t *f, const ifc_arm_t *arm,
                            int value) {
  if (!f || !arm)
    return false;
  for (size_t i = 0; i < arm->count; ++i)
    if (f->data[arm->insts[i]].dst == value)
      return true;
  return false;
}

static bool ifc_rewrite(nyr_func_t *f, size_t branch,
                        const ifc_arm_t *false_arm, const ifc_arm_t *true_arm,
                        const ifc_phi_pair_t *phis, size_t phi_count,
                        const ifc_store_pair_t *store_pairs,
                        size_t store_pair_count, int64_t join_label,
                        bool normalize_condition) {
  if (!f || !phis || phi_count == 0 || phi_count > 8)
    return false;
  size_t moved_count = 0;
  for (size_t i = 0; i < false_arm->count; ++i)
    moved_count += f->data[false_arm->insts[i]].op != NYIR_STORE_LOCAL;
  for (size_t i = 0; i < true_arm->count; ++i)
    moved_count += f->data[true_arm->insts[i]].op != NYIR_STORE_LOCAL;
  if (moved_count > 32)
    return false;
  size_t inserted = moved_count + 2 * store_pair_count + phi_count + 1 +
                    (normalize_condition ? 2 : 0);
  size_t delta = inserted - 1;
  nyr_inst_t moved[32];
  size_t n = 0;
  for (size_t i = 0; i < false_arm->count; ++i)
    if (f->data[false_arm->insts[i]].op != NYIR_STORE_LOCAL)
      moved[n++] = f->data[false_arm->insts[i]];
  for (size_t i = 0; i < true_arm->count; ++i)
    if (f->data[true_arm->insts[i]].op != NYIR_STORE_LOCAL)
      moved[n++] = f->data[true_arm->insts[i]];
  nyr_inst_t old_branch = f->data[branch];
  nyr_inst_t old_phis[8];
  for (size_t i = 0; i < phi_count; ++i)
    old_phis[i] = f->data[phis[i].phi_index];
  int condition = old_branch.a;
  int zero = -1;
  if (normalize_condition) {
    zero = f->next_value++;
    condition = f->next_value++;
  }
  int selected_stores[8];
  for (size_t i = 0; i < store_pair_count; ++i)
    selected_stores[i] = f->next_value++;
  if (!nir_ensure_inst_space(f, delta))
    return false;
  memmove(&f->data[branch + inserted], &f->data[branch + 1],
          (f->len - branch - 1) * sizeof(*f->data));
  f->len += delta;
  for (size_t i = 0; i < n; ++i)
    f->data[branch + i] = moved[i];
  size_t select_at = branch + n;
  if (normalize_condition) {
    f->data[select_at++] = (nyr_inst_t){.op = NYIR_CONST_I64,
                                        .dst = zero,
                                        .a = -1,
                                        .b = -1,
                                        .c = -1,
                                        .d = -1,
                                        .e = -1,
                                        .f = -1,
                                        .imm = 0};
    f->data[select_at++] = (nyr_inst_t){.op = NYIR_CMP_I64,
                                        .dst = condition,
                                        .a = old_branch.a,
                                        .b = zero,
                                        .c = -1,
                                        .d = -1,
                                        .e = -1,
                                        .f = -1,
                                        .cmp = NYIR_CMP_NE};
  }
  for (size_t i = 0; i < store_pair_count; ++i) {
    f->data[select_at++] = (nyr_inst_t){.op = NYIR_SELECT_I64,
                                        .dst = selected_stores[i],
                                        .a = condition,
                                        .b = store_pairs[i].true_value,
                                        .c = store_pairs[i].false_value,
                                        .d = -1,
                                        .e = -1,
                                        .f = -1,
                                        .scalar_class = NYIR_SCALAR_I64_RAW};
    f->data[select_at++] = (nyr_inst_t){.op = NYIR_STORE_LOCAL,
                                        .dst = -1,
                                        .a = selected_stores[i],
                                        .b = -1,
                                        .c = -1,
                                        .d = -1,
                                        .e = -1,
                                        .f = -1,
                                        .imm = store_pairs[i].slot,
                                        .scalar_class = NYIR_SCALAR_I64_RAW};
  }
  for (size_t i = 0; i < phi_count; ++i) {
    /*
     * The SELECT replaces the PHI result, so keep facts attached to that
     * result.  The incoming-edge allocation remains owned by the old PHI and
     * must not be copied to the scalar instruction.
     */
    nyr_inst_t select = old_phis[i];
    select.op = NYIR_SELECT_I64;
    select.scalar_class = NYIR_SCALAR_I64_RAW;
    select.a = condition;
    select.b = phis[i].true_value;
    select.c = phis[i].false_value;
    select.d = select.e = select.f = -1;
    select.imm = 0;
    select.phi_incoming = NULL;
    select.phi_incoming_len = 0;
    select.extra_args = NULL;
    select.extra_args_len = 0;
    select.arg_sizes = NULL;
    select.symbol = NULL;
    f->data[select_at++] = select;
  }
  f->data[select_at] = (nyr_inst_t){.op = NYIR_BR,
                                    .dst = -1,
                                    .a = -1,
                                    .b = -1,
                                    .c = -1,
                                    .d = -1,
                                    .e = -1,
                                    .f = -1,
                                    .imm = join_label,
                                    .debug = old_branch.debug};
  for (size_t i = 0; i < false_arm->count; ++i)
    (void)nyr_erase_instruction(f, false_arm->insts[i] + delta);
  for (size_t i = 0; i < true_arm->count; ++i)
    (void)nyr_erase_instruction(f, true_arm->insts[i] + delta);
  if (false_arm->branch != SIZE_MAX)
    (void)nyr_erase_instruction(f, false_arm->branch + delta);
  if (true_arm->branch != SIZE_MAX)
    (void)nyr_erase_instruction(f, true_arm->branch + delta);
  for (size_t i = 0; i < phi_count; ++i)
    (void)nyr_erase_instruction(f, phis[i].phi_index + delta);
  nyr_refresh_metadata(f);
  return true;
}

static bool ifc_block_has_phi(const nyr_func_t *f, const nyr_cfg_t *cfg,
                              size_t block) {
  if (!f || !cfg || block >= cfg->block_count)
    return true;
  for (size_t i = cfg->block_start[block]; i < cfg->block_end[block]; ++i)
    if (f->data[i].op == NYIR_PHI)
      return true;
  return false;
}

/*
 * A diamond whose arms assign the same mutable local has no join PHI: lowering
 * stores the slot on both arms. Speculate the same whitelisted arm operations
 * as the PHI form, then select the stored value and keep one store.
 */
static bool ifc_rewrite_store_only(nyr_func_t *f, size_t branch,
                                   const ifc_arm_t *false_arm,
                                   const ifc_arm_t *true_arm,
                                   const ifc_store_pair_t *store_pairs,
                                   size_t store_pair_count,
                                   int64_t join_label,
                                   bool normalize_condition) {
  if (!f || !false_arm || !true_arm || !store_pair_count)
    return false;
  size_t moved_count = 0;
  for (size_t i = 0; i < false_arm->count; ++i)
    moved_count += f->data[false_arm->insts[i]].op != NYIR_STORE_LOCAL;
  for (size_t i = 0; i < true_arm->count; ++i)
    moved_count += f->data[true_arm->insts[i]].op != NYIR_STORE_LOCAL;
  if (moved_count > 32)
    return false;
  size_t inserted = moved_count + 2 * store_pair_count + 1 +
                    (normalize_condition ? 2 : 0);
  size_t delta = inserted - 1;
  nyr_inst_t moved[32];
  size_t n = 0;
  for (size_t i = 0; i < false_arm->count; ++i)
    if (f->data[false_arm->insts[i]].op != NYIR_STORE_LOCAL)
      moved[n++] = f->data[false_arm->insts[i]];
  for (size_t i = 0; i < true_arm->count; ++i)
    if (f->data[true_arm->insts[i]].op != NYIR_STORE_LOCAL)
      moved[n++] = f->data[true_arm->insts[i]];
  nyr_inst_t old_branch = f->data[branch];
  int condition = old_branch.a;
  int zero = -1;
  if (normalize_condition) {
    zero = f->next_value++;
    condition = f->next_value++;
  }
  int selected_stores[8];
  for (size_t i = 0; i < store_pair_count; ++i)
    selected_stores[i] = f->next_value++;
  if (!nir_ensure_inst_space(f, delta))
    return false;
  memmove(&f->data[branch + inserted], &f->data[branch + 1],
          (f->len - branch - 1) * sizeof(*f->data));
  f->len += delta;
  for (size_t i = 0; i < n; ++i)
    f->data[branch + i] = moved[i];
  size_t at = branch + n;
  if (normalize_condition) {
    f->data[at++] = (nyr_inst_t){.op = NYIR_CONST_I64,
                                 .dst = zero,
                                 .a = -1,
                                 .b = -1,
                                 .c = -1,
                                 .d = -1,
                                 .e = -1,
                                 .f = -1,
                                 .imm = 0};
    f->data[at++] = (nyr_inst_t){.op = NYIR_CMP_I64,
                                 .dst = condition,
                                 .a = old_branch.a,
                                 .b = zero,
                                 .c = -1,
                                 .d = -1,
                                 .e = -1,
                                 .f = -1,
                                 .cmp = NYIR_CMP_NE};
  }
  for (size_t i = 0; i < store_pair_count; ++i) {
    f->data[at++] = (nyr_inst_t){.op = NYIR_SELECT_I64,
                                 .dst = selected_stores[i],
                                 .a = condition,
                                 .b = store_pairs[i].true_value,
                                 .c = store_pairs[i].false_value,
                                 .d = -1,
                                 .e = -1,
                                 .f = -1,
                                 .scalar_class = NYIR_SCALAR_I64_RAW};
    f->data[at++] = (nyr_inst_t){.op = NYIR_STORE_LOCAL,
                                 .dst = -1,
                                 .a = selected_stores[i],
                                 .b = -1,
                                 .c = -1,
                                 .d = -1,
                                 .e = -1,
                                 .f = -1,
                                 .imm = store_pairs[i].slot,
                                 .scalar_class = NYIR_SCALAR_I64_RAW};
  }
  f->data[at] = (nyr_inst_t){.op = NYIR_BR,
                             .dst = -1,
                             .a = -1,
                             .b = -1,
                             .c = -1,
                             .d = -1,
                             .e = -1,
                             .f = -1,
                             .imm = join_label,
                             .debug = old_branch.debug};
  for (size_t i = 0; i < false_arm->count; ++i)
    (void)nyr_erase_instruction(f, false_arm->insts[i] + delta);
  for (size_t i = 0; i < true_arm->count; ++i)
    (void)nyr_erase_instruction(f, true_arm->insts[i] + delta);
  if (false_arm->branch != SIZE_MAX)
    (void)nyr_erase_instruction(f, false_arm->branch + delta);
  if (true_arm->branch != SIZE_MAX)
    (void)nyr_erase_instruction(f, true_arm->branch + delta);
  nyr_refresh_metadata(f);
  return true;
}

/*
 * Raw-i64 provenance of an unclassified value: its definition is raw, or it
 * is a copy/PHI (a running max carried by the loop header PHI) whose every
 * source is raw. A cycle back to the PHI being converted is consistent.
 */
static bool ifc_value_raw(const nyr_func_t *f, const int *defs, int v,
                          int root, unsigned depth) {
  /* Only a cycle reached through another PHI may close on the root; an arm
   * value that is the join PHI itself would make the SELECT read its own
   * result. */
  if (v == root)
    return depth > 0;
  if (!f || !defs || v < 0 || v >= f->next_value || defs[v] < 0 ||
      depth > 6)
    return false;
  const nyr_inst_t *def = &f->data[(size_t)defs[v]];
  if (def->scalar_class == NYIR_SCALAR_I64_RAW)
    return true;
  if (def->scalar_class != NYIR_SCALAR_UNKNOWN ||
      getenv("NYTRIX_NO_IFCONV_PHI_RAW"))
    return false;
  /* An unclassified i64 immediate is a raw integer constant. */
  if (def->op == NYIR_CONST_I64 && !getenv("NYTRIX_NO_IFCONV_PHI_RAW"))
    return true;
  if (def->op == NYIR_COPY)
    return ifc_value_raw(f, defs, def->a, root, depth + 1);
  if (def->op != NYIR_PHI || !def->phi_incoming || !def->phi_incoming_len)
    return false;
  for (size_t k = 0; k < def->phi_incoming_len; ++k) {
    int in = def->phi_incoming[k].value;
    if (in != v && !ifc_value_raw(f, defs, in, root, depth + 1))
      return false;
  }
  return true;
}

bool nyr_if_convert(nyr_func_t *f) {
  if (!f || f->next_value <= 0)
    return true;
  for (;;) {
    nyr_cfg_t cfg = {0};
    int *defs = nyr_build_defs(f);
    nyr_type_map_t types = {0};
    if (!defs || !nyr_cfg_build(f, &cfg) ||
        !nyr_type_map_init(&types, f, nyr_max_local(f))) {
      free(defs);
      nyr_cfg_free(&cfg);
      nyr_type_map_free(&types);
      return false;
    }
    bool changed = false;
    for (size_t header = 0; header < cfg.block_count && !changed; ++header) {
      size_t end = cfg.block_end[header];
      while (end > cfg.block_start[header] && f->data[end - 1].op == NYIR_NOP)
        --end;
      if (!cfg.reachable[header])
        continue;
      if (end == cfg.block_start[header])
        continue;
      if (f->data[end - 1].op != NYIR_BR_IF)
        continue;
      if (cfg.succ_offsets[header + 1] - cfg.succ_offsets[header] != 2)
        continue;
      size_t branch = end - 1;
      const nyr_inst_t *br = &f->data[branch];
      size_t true_entry = ifc_label_block(&cfg, br->imm);
      size_t false_entry = SIZE_MAX;
      for (size_t edge = cfg.succ_offsets[header];
           edge < cfg.succ_offsets[header + 1]; ++edge) {
        size_t successor = cfg.succ_blocks[edge];
        if (successor == true_entry)
          continue;
        if (false_entry != SIZE_MAX) {
          false_entry = SIZE_MAX;
          break;
        }
        false_entry = successor;
      }
      size_t false_block = false_entry;
      size_t true_block = true_entry;
      if (false_block < cfg.block_count)
        false_block = ifc_resolve_empty_bridges(f, &cfg, false_block);
      if (true_block != SIZE_MAX)
        true_block = ifc_resolve_empty_bridges(f, &cfg, true_block);
      bool condition_is_boolean = ifc_boolean_condition(f, defs, br->a, 0);
      bool raw_condition =
          br->a >= 0 && br->a < f->next_value && defs[br->a] >= 0 &&
          f->data[(size_t)defs[br->a]].scalar_class == NYIR_SCALAR_I64_RAW;
      if (!condition_is_boolean && !raw_condition)
        continue;
      /*
       * Both arms empty (`if c { x = a }` lowered to two bridge blocks that
       * only branch to the join): bridge resolution lands both on the join.
       * The join PHI alone selects the value.
       */
      bool both_empty = false_block < cfg.block_count &&
                        true_block == false_block && true_block > header &&
                        false_entry != SIZE_MAX && true_entry != SIZE_MAX &&
                        true_entry != false_entry &&
                        !getenv("NYTRIX_NO_IFCONV_BOTH_EMPTY");
      if (!both_empty &&
          (false_block >= cfg.block_count || true_block == SIZE_MAX ||
           true_block == false_block || true_block <= header ||
           false_entry == SIZE_MAX))
        continue;
      size_t false_count =
          cfg.succ_offsets[false_block + 1] - cfg.succ_offsets[false_block];
      size_t true_count =
          cfg.succ_offsets[true_block + 1] - cfg.succ_offsets[true_block];
      /*
       * An arm that only branches to the join is a legal empty arm.  Bridge
       * resolution walks such an entry straight into the join, so the join is
       * whatever the other arm branches to and the arm itself is the last
       * bridge block before it.
       */
      size_t false_arm_block = false_block;
      size_t true_arm_block = true_block;
      bool false_empty = false, true_empty = false;
      size_t join;
      if (both_empty) {
        join = true_block;
        false_empty = true_empty = true;
      } else if (true_count == 1 &&
          cfg.succ_blocks[cfg.succ_offsets[true_block]] == false_block) {
        join = false_block;
        false_empty = true;
      } else if (false_count == 1 &&
                 cfg.succ_blocks[cfg.succ_offsets[false_block]] == true_block) {
        join = true_block;
        true_empty = true;
      } else if (false_count == 1 && true_count == 1 &&
                 cfg.succ_blocks[cfg.succ_offsets[false_block]] ==
                     cfg.succ_blocks[cfg.succ_offsets[true_block]]) {
        join = cfg.succ_blocks[cfg.succ_offsets[false_block]];
      } else {
        continue;
      }
      if (join >= cfg.block_count)
        continue;
      if (false_empty) {
        if (!ifc_exclusive_bridge(f, &cfg, header, false_entry, join,
                                  &false_arm_block))
          continue;
      } else if (!ifc_exclusive_edge(f, &cfg, header, false_entry, false_block)) {
        continue;
      }
      if (true_empty) {
        if (!ifc_exclusive_bridge(f, &cfg, header, true_entry, join,
                                  &true_arm_block))
          continue;
      } else if (!ifc_exclusive_edge(f, &cfg, header, true_entry, true_block)) {
        continue;
      }
      if (false_arm_block == join || true_arm_block == join)
        continue;
      int64_t join_label = cfg.block_label[join];
      if (join_label < 0 || !ifc_two_predecessors(&cfg, join, false_arm_block,
                                                  true_arm_block))
        continue;
      ifc_arm_t false_arm, true_arm;
      ifc_store_pair_t store_pairs[8];
      size_t store_pair_count = 0;
      ifc_phi_pair_t phis[8];
      size_t phi_count = 0;
      if (!ifc_parse_arm(f, &cfg, defs, false_arm_block, header, branch,
                         join_label, &false_arm) ||
          !ifc_parse_arm(f, &cfg, defs, true_arm_block, header, branch,
                         join_label, &true_arm) ||
          !ifc_arm_store_pairs(f, &false_arm, &true_arm, store_pairs,
                               &store_pair_count) ||
          !ifc_arms_have_stable_local_loads(f, &cfg, header, branch, &false_arm,
                                            &true_arm))
        continue;
      bool have_phi = ifc_join_phis(f, &cfg, join, &false_arm, &true_arm, phis,
                                    &phi_count);
      for (size_t i = 0; have_phi && i < phi_count; ++i) {
        if (!ifc_arm_value_ok(f, &cfg, defs, &false_arm, header, branch,
                              phis[i].false_value) ||
            !ifc_arm_value_ok(f, &cfg, defs, &true_arm, header, branch,
                              phis[i].true_value)) {
          have_phi = false;
          break;
        }
        const nyr_inst_t *phi_inst = &f->data[phis[i].phi_index];
        /*
         * Loop collapsing can leave an unclassified PHI whose incoming
         * definitions still carry exact raw-i64 ABI facts. Recover only that
         * unambiguous class; tagged values and pointers remain excluded.
         */
        bool raw_phi = phi_inst->scalar_class == NYIR_SCALAR_I64_RAW;
        if (phi_inst->scalar_class == NYIR_SCALAR_UNKNOWN) {
          /*
           * One arm must be a proven raw-i64 definition; the other may reach
           * raw values through constants, copies and loop PHIs (a running
           * max). Two unclassified arms can be pointers carried through PHIs
           * and stay as branches.
           */
          int fv = phis[i].false_value, tv = phis[i].true_value;
          bool f_direct = defs[fv] >= 0 &&
                          f->data[defs[fv]].scalar_class == NYIR_SCALAR_I64_RAW;
          bool t_direct = defs[tv] >= 0 &&
                          f->data[defs[tv]].scalar_class == NYIR_SCALAR_I64_RAW;
          raw_phi = (f_direct || t_direct) &&
                    ifc_value_raw(f, defs, fv, phi_inst->dst, 0) &&
                    ifc_value_raw(f, defs, tv, phi_inst->dst, 0);
        }
        if (!raw_phi || types.value_f64[phis[i].false_value] ||
            types.value_f32[phis[i].false_value] ||
            types.value_f64[phis[i].true_value] ||
            types.value_f32[phis[i].true_value])
          have_phi = false;
      }
      bool store_only =
          !have_phi && store_pair_count > 0 && !ifc_block_has_phi(f, &cfg, join);
      if (!have_phi && !store_only)
        continue;
      size_t cost = ifc_arm_cost(f, &false_arm) + ifc_arm_cost(f, &true_arm);
      size_t budget =
          cost > 4 && ifc_diamond_is_in_loop(&cfg, header, false_block,
                                             true_block, join)
              ? 20
              : 4;
      if (cost > budget)
        continue;
      bool rewritten = false;
      if (have_phi) {
        rewritten = ifc_rewrite(f, branch, &false_arm, &true_arm, phis,
                                phi_count, store_pairs, store_pair_count,
                                join_label, !condition_is_boolean);
      } else {
        rewritten = ifc_rewrite_store_only(
            f, branch, &false_arm, &true_arm, store_pairs, store_pair_count,
            join_label, !condition_is_boolean);
      }
      if (!rewritten) {
        free(defs);
        nyr_cfg_free(&cfg);
        nyr_type_map_free(&types);
        return false;
      }
      changed = true;
    }
    free(defs);
    nyr_cfg_free(&cfg);
    nyr_type_map_free(&types);
    if (!changed)
      return true;
  }
}
