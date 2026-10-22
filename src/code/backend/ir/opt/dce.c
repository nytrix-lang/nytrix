/*
 * Dead-code elimination: removes unused instructions and unreachable
 * basic blocks from NYIR after other passes produce dead values.
 */
#include "base/common.h"
#include "base/compat.h"
#include "code/backend/ir/internal.h"
#include "code/backend/ir/opt/util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void dce_mark_value(int value, size_t nv, bool *used, int *work,
                           size_t *work_len) {
  if (value >= 0 && (size_t)value < nv && !used[value]) {
    used[value] = true;
    work[(*work_len)++] = value;
  }
}

static void dce_mark_operands(const nyr_inst_t *in, size_t nv, bool *used,
                              int *work, size_t *work_len) {
  dce_mark_value(in->a, nv, used, work, work_len);
  dce_mark_value(in->b, nv, used, work, work_len);
  dce_mark_value(in->c, nv, used, work, work_len);
  dce_mark_value(in->d, nv, used, work, work_len);
  dce_mark_value(in->e, nv, used, work, work_len);
  dce_mark_value(in->f, nv, used, work, work_len);
  for (size_t k = 0; k < in->extra_args_len; ++k)
    dce_mark_value(in->extra_args[k], nv, used, work, work_len);
  for (size_t k = 0; k < in->phi_incoming_len; ++k)
    dce_mark_value(in->phi_incoming[k].value, nv, used, work, work_len);
}

static void dce_mark_operands_linear(const nyr_inst_t *in, size_t nv,
                                     bool *used) {
  const int fixed[] = {in->a, in->b, in->c, in->d, in->e, in->f};
  for (size_t k = 0; k < sizeof(fixed) / sizeof(fixed[0]); ++k)
    if (fixed[k] >= 0 && (size_t)fixed[k] < nv)
      used[fixed[k]] = true;
  for (size_t k = 0; k < in->extra_args_len; ++k) {
    int value = in->extra_args[k];
    if (value >= 0 && (size_t)value < nv)
      used[value] = true;
  }
  for (size_t k = 0; k < in->phi_incoming_len; ++k) {
    int value = in->phi_incoming[k].value;
    if (value >= 0 && (size_t)value < nv)
      used[value] = true;
  }
}

bool nyr_dce(nyr_func_t *f) {
  if (!f || f->next_value <= 0)
    return true;
  size_t nv = (size_t)f->next_value;
  if (nv > SIZE_MAX / sizeof(int))
    return false;
  bool has_phi = false;
  bool has_control_flow = false;
  bool non_nop_after = false;
  bool return_with_live_tail = false;
  for (size_t i = f->len; i > 0; --i) {
    nyr_op_t op = f->data[i - 1].op;
    has_phi |= op == NYIR_PHI;
    has_control_flow |= op == NYIR_LABEL || op == NYIR_BR || op == NYIR_BR_IF;
    if (op == NYIR_RET && non_nop_after)
      return_with_live_tail = true;
    non_nop_after |= op != NYIR_NOP;
  }
  bool stk_used[256] = {0};
  bool *used = nv <= 256 ? stk_used : (bool *)calloc(nv, sizeof(bool));
  int *work = has_phi ? (int *)malloc(nv * sizeof(*work)) : NULL;
  if (!used || (has_phi && !work)) {
    if (nv > 256)
      free(used);
    free(work);
    return false;
  }

  /*
   * A label referenced only by an already-dead block must not keep its target
   * alive.  Use the canonical CFG instead of the former linear heuristic,
   * which could retain chains reachable solely from unreachable code.
   */
  bool needs_reachability =
      has_phi || has_control_flow || return_with_live_tail;
  if (needs_reachability) {
    nyr_cfg_t cfg = {0};
    if (!nyr_cfg_build_topology(f, &cfg)) {
      if (nv > 256)
        free(used);
      free(work);
      return false;
    }
    /*
     * Drop PHI operands from dead predecessors while the original CFG still
     * describes those edges. Erasing dead terminators first can turn dead
     * blocks into fallthrough blocks and make the PHI edge set inconsistent.
     */
    for (size_t i = 0; i < f->len; ++i) {
      nyr_inst_t *phi = &f->data[i];
      if (phi->op != NYIR_PHI)
        continue;
      size_t block = cfg.inst_block[i];
      if (!cfg.reachable[block])
        continue;
      size_t out = 0;
      for (size_t k = 0; k < phi->phi_incoming_len; ++k) {
        bool live = false;
        for (size_t edge = cfg.pred_offsets[block];
             edge < cfg.pred_offsets[block + 1]; ++edge) {
          size_t pred = cfg.pred_blocks[edge];
          if (cfg.reachable[pred] &&
              cfg.block_label[pred] == phi->phi_incoming[k].predecessor_label) {
            live = true;
            break;
          }
        }
        if (live)
          phi->phi_incoming[out++] = phi->phi_incoming[k];
      }
      phi->phi_incoming_len = out;
    }
    for (size_t i = 0; i < f->len; ++i) {
      if (cfg.inst_block && !cfg.reachable[cfg.inst_block[i]])
        (void)nyr_erase_instruction(f, i);
    }
    nyr_cfg_free(&cfg);
    if (!nyr_compact(f)) {
      if (nv > 256)
        free(used);
      free(work);
      return false;
    }
  }
  if (!nyr_prune_phis(f)) {
    if (nv > 256)
      free(used);
    free(work);
    return false;
  }

  int64_t max_label = -1;
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if ((in->op == NYIR_LABEL || in->op == NYIR_BR || in->op == NYIR_BR_IF) &&
        in->imm >= 0 && in->imm > max_label)
      max_label = in->imm;
    for (size_t k = 0; k < in->phi_incoming_len; ++k)
      if (in->phi_incoming[k].predecessor_label > max_label)
        max_label = in->phi_incoming[k].predecessor_label;
  }
  bool *label_referenced = NULL;
  bool stk_label_ref[256] = {0};
  size_t ml_sz = (size_t)max_label + 1u;
  if (max_label >= 0 && (uint64_t)max_label <= (uint64_t)f->len * 4u + 1024u) {
    label_referenced = ml_sz <= 256
                           ? stk_label_ref
                           : (bool *)calloc(ml_sz, sizeof(*label_referenced));
    if (!label_referenced) {
      if (nv > 256)
        free(used);
      free(work);
      return false;
    }
    for (size_t i = 0; i < f->len; ++i) {
      const nyr_inst_t *in = &f->data[i];
      if ((in->op == NYIR_BR || in->op == NYIR_BR_IF) && in->imm >= 0 &&
          in->imm <= max_label)
        label_referenced[in->imm] = true;
      for (size_t k = 0; k < in->phi_incoming_len; ++k) {
        int64_t label = in->phi_incoming[k].predecessor_label;
        if (label >= 0 && label <= max_label)
          label_referenced[label] = true;
      }
    }
  }
#define NIR_LABEL_REFERENCED(label)                                            \
  (label_referenced && (label) >= 0 && (label) <= max_label                    \
       ? label_referenced[(size_t)(label)]                                     \
       : nyr_label_referenced(f, (label)))

  if (!has_phi) {
    /*
     * Acyclic SSA is already definition ordered; one reverse sweep suffices.
     */
    for (size_t i = f->len; i > 0; --i) {
      nyr_inst_t *in = &f->data[i - 1];
      unsigned eff = nyr_effective_effects(in);
      bool keep = (eff & ~(NYIR_EFFECT_CALL | NYIR_EFFECT_ALLOCATION |
                           NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_READ_LOCAL)) !=
                      NYIR_EFFECT_NONE ||
                  in->op == NYIR_RET || in->op == NYIR_BR ||
                  in->op == NYIR_BR_IF ||
                  (in->op == NYIR_LABEL && NIR_LABEL_REFERENCED(in->imm)) ||
                  (in->dst >= 0 && used[in->dst]);
      if (keep)
        dce_mark_operands_linear(in, nv, used);
    }
  } else {
    /*
     * Seed from observable instructions and follow SSA definitions. Each
     * value enters once, so cyclic PHI webs need no whole-function rescans.
     */
    size_t work_len = 0;
    for (size_t i = 0; i < f->len; ++i) {
      nyr_inst_t *in = &f->data[i];
      unsigned eff = nyr_effective_effects(in);
      bool side_effect =
          (eff & ~(NYIR_EFFECT_CALL | NYIR_EFFECT_ALLOCATION |
                   NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_READ_LOCAL)) !=
              NYIR_EFFECT_NONE ||
          in->op == NYIR_RET || in->op == NYIR_BR || in->op == NYIR_BR_IF;
      if (in->op == NYIR_LABEL)
        side_effect = NIR_LABEL_REFERENCED(in->imm);
      if (side_effect)
        dce_mark_operands(in, nv, used, work, &work_len);
    }
    if (nv > SIZE_MAX / sizeof(size_t) || f->len > SIZE_MAX / sizeof(size_t)) {
      if (ml_sz > 256)
        free(label_referenced);
      if (nv > 256)
        free(used);
      free(work);
      return false;
    }
    size_t *def_head = (size_t *)malloc(nv * sizeof(*def_head));
    size_t *def_next =
        f->len ? (size_t *)malloc(f->len * sizeof(*def_next)) : NULL;
    if (!def_head || (f->len && !def_next)) {
      free(def_head);
      free(def_next);
      if (ml_sz > 256)
        free(label_referenced);
      if (nv > 256)
        free(used);
      free(work);
      return false;
    }
    for (size_t v = 0; v < nv; ++v)
      def_head[v] = SIZE_MAX;
    for (size_t i = 0; i < f->len; ++i) {
      int value = f->data[i].dst;
      if (value >= 0 && (size_t)value < nv) {
        def_next[i] = def_head[value];
        def_head[value] = i;
      } else {
        def_next[i] = SIZE_MAX;
      }
    }
    for (size_t head = 0; head < work_len; ++head) {
      int value = work[head];
      for (size_t def = def_head[value]; def != SIZE_MAX; def = def_next[def]) {
        dce_mark_operands(&f->data[def], nv, used, work, &work_len);
      }
    }
    free(def_head);
    free(def_next);
  }

  for (size_t i = f->len; i > 0; --i) {
    nyr_inst_t *in = &f->data[i - 1];
    unsigned eff = nyr_effective_effects(in);
    bool side_effect =
        (eff & ~(NYIR_EFFECT_CALL | NYIR_EFFECT_ALLOCATION |
                 NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_READ_LOCAL)) !=
            NYIR_EFFECT_NONE ||
        in->op == NYIR_RET || in->op == NYIR_BR || in->op == NYIR_BR_IF;
    if (in->op == NYIR_LABEL)
      side_effect = NIR_LABEL_REFERENCED(in->imm);
    if (!side_effect && !(in->dst >= 0 && used[in->dst]))
      (void)nyr_erase_instruction(f, i - 1);
  }
  if (ml_sz > 256)
    free(label_referenced);
  if (nv > 256)
    free(used);
  free(work);
#undef NIR_LABEL_REFERENCED
  return true;
}
