/*
 * Dead-code elimination: removes unused instructions and unreachable
 * basic blocks from NYIR after other passes produce dead values.
 */
#include "code/native/ir/opt/util.h"
#include "code/native/ir/internal.h"
#include "base/compat.h"
#include "base/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool nyir_dce(nyir_func_t *f) {
  if (!f || f->next_value <= 0)
    return true;
  /*
   * A reverse linear liveness sweep cannot soundly eliminate cyclic SSA:
   * a loop-header PHI can be visited before the side-effecting use that makes
   * its result live.  Iterate to a fixed point whenever PHIs are present.
   */
  bool has_phi = false;
  for (size_t i = 0; i < f->len; ++i)
    if (f->data[i].op == NYIR_PHI) {
      has_phi = true;
      break;
    }
  size_t nv = (size_t)f->next_value;
  bool stk_used[256] = {0};
  bool *used = nv <= 256 ? stk_used : (bool *)calloc(nv, sizeof(bool));
  if (!used)
    return false;

  /*
   * A label referenced only by an already-dead block must not keep its target
   * alive.  Use the canonical CFG instead of the former linear heuristic,
   * which could retain chains reachable solely from unreachable code.
   */
  nyir_cfg_t cfg = {0};
  if (!nyir_cfg_build_topology(f, &cfg)) {
    if (nv > 256) free(used);
    return false;
  }
  for (size_t i = 0; i < f->len; ++i) {
    if (cfg.inst_block && !cfg.reachable[cfg.inst_block[i]])
      (void)nyir_erase_instruction(f, i);
  }
  nyir_cfg_free(&cfg);
  if (!nyir_prune_phis(f)) {
    if (nv > 256) free(used);
    return false;
  }

  int64_t max_label = -1;
  for (size_t i = 0; i < f->len; ++i) {
    const nyir_inst_t *in = &f->data[i];
    if ((in->op == NYIR_LABEL || in->op == NYIR_BR ||
         in->op == NYIR_BR_IF) &&
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
    label_referenced = ml_sz <= 256 ? stk_label_ref : (bool *)calloc(ml_sz, sizeof(*label_referenced));
    if (!label_referenced) {
      if (nv > 256) free(used);
      return false;
    }
    for (size_t i = 0; i < f->len; ++i) {
      const nyir_inst_t *in = &f->data[i];
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
       : nyir_label_referenced(f, (label)))

  /*
   * A loop-header PHI can be visited before the side-effecting use that makes
   * its result live. Propagate liveness to a fixed point before erasing.
   */
  bool changed;
  do {
    changed = false;
    for (size_t i = f->len; i > 0; --i) {
      nyir_inst_t *in = &f->data[i - 1];
      unsigned eff = nyir_effective_effects(in);
      bool side_effect =
          (eff & ~(NYIR_EFFECT_CALL | NYIR_EFFECT_ALLOCATION)) != NYIR_EFFECT_NONE ||
                         in->op == NYIR_RET || in->op == NYIR_BR ||
                         in->op == NYIR_BR_IF;
      if (in->op == NYIR_LABEL)
        side_effect = NIR_LABEL_REFERENCED(in->imm);
      bool keep = side_effect || (in->dst >= 0 && used[in->dst]);
      if (!keep)
        continue;
      int *operands[] = {&in->a, &in->b, &in->c,
                         &in->d, &in->e, &in->f};
      for (size_t k = 0; k < sizeof(operands) / sizeof(operands[0]); ++k)
        if (*operands[k] >= 0 && !used[*operands[k]]) {
          used[*operands[k]] = true;
          changed = true;
        }
      for (size_t k = 0; k < in->extra_args_len; ++k)
        if (in->extra_args[k] >= 0 && !used[in->extra_args[k]]) {
          used[in->extra_args[k]] = true;
          changed = true;
        }
      for (size_t k = 0; k < in->phi_incoming_len; ++k)
        if (in->phi_incoming[k].value >= 0 &&
            !used[in->phi_incoming[k].value]) {
          used[in->phi_incoming[k].value] = true;
          changed = true;
        }
    }
  } while (has_phi && changed);

  for (size_t i = f->len; i > 0; --i) {
    nyir_inst_t *in = &f->data[i - 1];
    unsigned eff = nyir_effective_effects(in);
    bool side_effect =
        (eff & ~(NYIR_EFFECT_CALL | NYIR_EFFECT_ALLOCATION)) != NYIR_EFFECT_NONE ||
                       in->op == NYIR_RET || in->op == NYIR_BR ||
                       in->op == NYIR_BR_IF;
    if (in->op == NYIR_LABEL)
      side_effect = NIR_LABEL_REFERENCED(in->imm);
    if (!side_effect && !(in->dst >= 0 && used[in->dst]))
      (void)nyir_erase_instruction(f, i - 1);
  }
  if (ml_sz > 256) free(label_referenced);
  if (nv > 256) free(used);
#undef NIR_LABEL_REFERENCED
  return true;
}
