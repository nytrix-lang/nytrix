/*
 * Exact i64 diamond if-conversion.
 *
 * The pass only speculates a whitelist of non-trapping scalar operations.  It
 * requires header -> {false,true} -> join with two exclusive predecessors and
 * exactly one join PHI, then moves both bounded arms before SELECT_I64.
 */
#include "code/ir/opt/util.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
  size_t insts[3];
  size_t count;
  size_t branch;
  int value;
  int64_t label;
} ifc_arm_t;

static size_t ifc_label_block(const nyir_cfg_t *cfg, int64_t label) {
  if (!cfg)
    return SIZE_MAX;
  for (size_t block = 0; block < cfg->block_count; ++block)
    if (cfg->block_label[block] == label)
      return block;
  return SIZE_MAX;
}

static bool ifc_single_predecessor(const nyir_cfg_t *cfg, size_t block,
                                   size_t predecessor) {
  return cfg && block < cfg->block_count &&
         cfg->pred_offsets[block + 1] - cfg->pred_offsets[block] == 1 &&
         cfg->pred_blocks[cfg->pred_offsets[block]] == predecessor;
}

static bool ifc_two_predecessors(const nyir_cfg_t *cfg, size_t block,
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

static bool ifc_boolean_condition(const nyir_func_t *f, const int *defs,
                                  int value, unsigned depth) {
  if (!f || !defs || value < 0 || value >= f->next_value || depth > 16 ||
      defs[value] < 0)
    return false;
  const nyir_inst_t *def = &f->data[(size_t)defs[value]];
  if (def->op == NYIR_CMP_I64 || def->op == NYIR_CMP_F64 ||
      def->op == NYIR_CMP_F32)
    return true;
  if (def->op == NYIR_CONST_I64)
    return def->imm == 0 || def->imm == 1;
  return def->op == NYIR_COPY &&
         ifc_boolean_condition(f, defs, def->a, depth + 1);
}

static bool ifc_safe_arm_op(const nyir_inst_t *in) {
  if (!in || in->dst < 0 || nyir_inst_effects(in) != NYIR_EFFECT_NONE ||
      in->extra_args || in->extra_args_len || in->arg_sizes ||
      in->phi_incoming || in->phi_incoming_len || in->symbol)
    return false;
  switch (in->op) {
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
  case NYIR_CMP_I64:
    return true;
  default:
    return false;
  }
}

static bool ifc_value_available(const nyir_cfg_t *cfg, const int *defs,
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
         (def_block != header && nyir_cfg_dominates(cfg, def_block, header));
}

static bool ifc_arm_operands_available(const nyir_cfg_t *cfg, const int *defs,
                                       const ifc_arm_t *arm, size_t header,
                                       size_t header_branch,
                                       const nyir_inst_t *in) {
  if (in->op == NYIR_CONST_I64)
    return true;
  if (!ifc_value_available(cfg, defs, arm, header, header_branch, in->a))
    return false;
  if (in->op == NYIR_COPY)
    return true;
  return ifc_value_available(cfg, defs, arm, header, header_branch, in->b);
}

static bool ifc_parse_arm(const nyir_func_t *f, const nyir_cfg_t *cfg,
                          const int *defs, size_t block, size_t header,
                          size_t header_branch, int64_t join_label,
                          ifc_arm_t *arm) {
  if (!f || !cfg || !defs || !arm || block >= cfg->block_count ||
      cfg->block_label[block] < 0 ||
      cfg->succ_offsets[block + 1] - cfg->succ_offsets[block] != 1 ||
      cfg->block_label[cfg->succ_blocks[cfg->succ_offsets[block]]] != join_label)
    return false;
  *arm = (ifc_arm_t){.branch = SIZE_MAX, .value = -1,
                      .label = cfg->block_label[block]};
  for (size_t i = cfg->block_start[block]; i < cfg->block_end[block]; ++i) {
    const nyir_inst_t *in = &f->data[i];
    if (in->op == NYIR_LABEL || in->op == NYIR_NOP)
      continue;
    if (in->op == NYIR_BR) {
      if (in->imm != join_label || arm->branch != SIZE_MAX)
        return false;
      arm->branch = i;
      continue;
    }
    if (arm->branch != SIZE_MAX || arm->count == 3 || !ifc_safe_arm_op(in) ||
        !ifc_arm_operands_available(cfg, defs, arm, header, header_branch, in))
      return false;
    arm->insts[arm->count++] = i;
  }
  return arm->count != 0 && arm->branch != SIZE_MAX;
}

static bool ifc_join_phi(const nyir_func_t *f, const nyir_cfg_t *cfg,
                         size_t join, const ifc_arm_t *false_arm,
                         const ifc_arm_t *true_arm, size_t *phi_index,
                         int *false_value, int *true_value) {
  if (!f || !cfg || !false_arm || !true_arm || !phi_index || !false_value ||
      !true_value || join >= cfg->block_count || cfg->block_label[join] < 0)
    return false;
  size_t found = SIZE_MAX;
  for (size_t i = cfg->block_start[join]; i < cfg->block_end[join]; ++i) {
    if (f->data[i].op != NYIR_PHI)
      continue;
    if (found != SIZE_MAX)
      return false;
    found = i;
  }
  if (found == SIZE_MAX)
    return false;
  const nyir_inst_t *phi = &f->data[found];
  if (phi->dst < 0 || phi->phi_incoming_len != 2 || !phi->phi_incoming)
    return false;
  *false_value = -1;
  *true_value = -1;
  for (size_t i = 0; i < phi->phi_incoming_len; ++i) {
    if (phi->phi_incoming[i].predecessor_label == false_arm->label)
      *false_value = phi->phi_incoming[i].value;
    else if (phi->phi_incoming[i].predecessor_label == true_arm->label)
      *true_value = phi->phi_incoming[i].value;
    else
      return false;
  }
  if (*false_value < 0 || *true_value < 0)
    return false;
  *phi_index = found;
  return true;
}

static bool ifc_arm_defines(const nyir_func_t *f, const ifc_arm_t *arm,
                            int value) {
  if (!f || !arm)
    return false;
  for (size_t i = 0; i < arm->count; ++i)
    if (f->data[arm->insts[i]].dst == value)
      return true;
  return false;
}

static bool ifc_rewrite(nyir_func_t *f, size_t branch, const ifc_arm_t *false_arm,
                        const ifc_arm_t *true_arm, size_t phi_index,
                        int false_value, int true_value, int64_t join_label) {
  size_t inserted = false_arm->count + true_arm->count + 2;
  size_t delta = inserted - 1;
  nyir_inst_t moved[6];
  size_t n = 0;
  for (size_t i = 0; i < false_arm->count; ++i)
    moved[n++] = f->data[false_arm->insts[i]];
  for (size_t i = 0; i < true_arm->count; ++i)
    moved[n++] = f->data[true_arm->insts[i]];
  nyir_inst_t old_branch = f->data[branch];
  nyir_inst_t phi = f->data[phi_index];
  if (!nir_ensure_inst_space(f, delta))
    return false;
  memmove(&f->data[branch + inserted], &f->data[branch + 1],
          (f->len - branch - 1) * sizeof(*f->data));
  f->len += delta;
  for (size_t i = 0; i < n; ++i)
    f->data[branch + i] = moved[i];
  f->data[branch + n] = (nyir_inst_t){
      .op = NYIR_SELECT_I64, .dst = phi.dst, .a = old_branch.a,
      .b = true_value, .c = false_value, .d = -1, .e = -1, .f = -1,
      .debug = old_branch.debug};
  f->data[branch + n + 1] = (nyir_inst_t){
      .op = NYIR_BR, .dst = -1, .a = -1, .b = -1, .c = -1, .d = -1, .e = -1,
      .f = -1, .imm = join_label, .debug = old_branch.debug};
  for (size_t i = 0; i < false_arm->count; ++i)
    (void)nyir_erase_instruction(f, false_arm->insts[i] + delta);
  for (size_t i = 0; i < true_arm->count; ++i)
    (void)nyir_erase_instruction(f, true_arm->insts[i] + delta);
  (void)nyir_erase_instruction(f, false_arm->branch + delta);
  (void)nyir_erase_instruction(f, true_arm->branch + delta);
  (void)nyir_erase_instruction(f, phi_index + delta);
  nyir_refresh_metadata(f);
  return true;
}

bool nyir_if_convert(nyir_func_t *f) {
  if (!f || f->next_value <= 0)
    return true;
  for (;;) {
    nyir_cfg_t cfg = {0};
    int *defs = nyir_build_defs(f);
    if (!defs || !nyir_cfg_build(f, &cfg)) {
      free(defs);
      nyir_cfg_free(&cfg);
      return false;
    }
    bool changed = false;
    for (size_t header = 0; header < cfg.block_count && !changed; ++header) {
      size_t end = cfg.block_end[header];
      while (end > cfg.block_start[header] && f->data[end - 1].op == NYIR_NOP)
        --end;
      if (!cfg.reachable[header] || end == cfg.block_start[header] ||
          f->data[end - 1].op != NYIR_BR_IF ||
          cfg.succ_offsets[header + 1] - cfg.succ_offsets[header] != 2)
        continue;
      size_t branch = end - 1;
      const nyir_inst_t *br = &f->data[branch];
      size_t false_block = header + 1;
      size_t true_block = ifc_label_block(&cfg, br->imm);
      if (!ifc_boolean_condition(f, defs, br->a, 0) ||
          false_block >= cfg.block_count || true_block == SIZE_MAX ||
          true_block == false_block || true_block <= header ||
          !ifc_single_predecessor(&cfg, false_block, header) ||
          !ifc_single_predecessor(&cfg, true_block, header))
        continue;
      if (cfg.succ_offsets[false_block + 1] - cfg.succ_offsets[false_block] != 1 ||
          cfg.succ_offsets[true_block + 1] - cfg.succ_offsets[true_block] != 1)
        continue;
      size_t false_successor = cfg.succ_blocks[cfg.succ_offsets[false_block]];
      if (cfg.succ_blocks[cfg.succ_offsets[true_block]] != false_successor ||
          false_successor <= true_block || false_successor >= cfg.block_count)
        continue;
      size_t join = false_successor;
      int64_t join_label = cfg.block_label[join];
      if (join_label < 0 ||
          !ifc_two_predecessors(&cfg, join, false_block, true_block))
        continue;
      ifc_arm_t false_arm, true_arm;
      size_t phi = SIZE_MAX;
      int false_value = -1, true_value = -1;
      if (!ifc_parse_arm(f, &cfg, defs, false_block, header, branch, join_label,
                         &false_arm) ||
          !ifc_parse_arm(f, &cfg, defs, true_block, header, branch, join_label,
                         &true_arm) ||
          !ifc_join_phi(f, &cfg, join, &false_arm, &true_arm, &phi,
                        &false_value, &true_value) ||
          !ifc_arm_defines(f, &false_arm, false_value) ||
          !ifc_arm_defines(f, &true_arm, true_value))
        continue;
      if (!ifc_rewrite(f, branch, &false_arm, &true_arm, phi, false_value,
                       true_value, join_label)) {
        free(defs);
        nyir_cfg_free(&cfg);
        return false;
      }
      changed = true;
    }
    free(defs);
    nyir_cfg_free(&cfg);
    if (!changed)
      return true;
  }
}
