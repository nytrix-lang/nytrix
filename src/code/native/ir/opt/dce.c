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
  bool *used = (bool *)calloc((size_t)f->next_value, sizeof(bool));
  if (!used)
    return false;

  /* A label referenced only by an already-dead block must not keep its target
   * alive.  Use the canonical CFG instead of the former linear heuristic,
   * which could retain chains reachable solely from unreachable code. */
  nyir_cfg_t cfg = {0};
  if (!nyir_cfg_build_topology(f, &cfg)) {
    free(used);
    return false;
  }
  for (size_t i = 0; i < f->len; ++i) {
    if (cfg.inst_block && !cfg.reachable[cfg.inst_block[i]])
      (void)nyir_erase_instruction(f, i);
  }
  nyir_cfg_free(&cfg);
  if (!nyir_prune_phis(f)) {
    free(used);
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
  if (max_label >= 0 && (uint64_t)max_label <= (uint64_t)f->len * 4u + 1024u) {
    label_referenced =
        (bool *)calloc((size_t)max_label + 1u, sizeof(*label_referenced));
    if (!label_referenced) {
      free(used);
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

  /* Backedge values are defined after loop-header PHIs in linear order. Seed
   * PHI operands before the reverse scan so cyclic SSA uses stay live. */
  for (size_t i = 0; i < f->len; ++i) {
    const nyir_inst_t *in = &f->data[i];
    if (in->op != NYIR_PHI)
      continue;
    for (size_t k = 0; k < in->phi_incoming_len; ++k)
      if (in->phi_incoming[k].value >= 0)
        used[in->phi_incoming[k].value] = true;
  }

  for (size_t i = f->len; i > 0; --i) {
    nyir_inst_t *in = &f->data[i - 1];
    bool side_effect = in->effects != NYIR_EFFECT_NONE ||
                       in->op == NYIR_RET || in->op == NYIR_BR ||
                       in->op == NYIR_BR_IF;
    if (in->op == NYIR_LABEL)
      side_effect = NIR_LABEL_REFERENCED(in->imm);
    bool keep = side_effect || (in->dst >= 0 && used[in->dst]);
    if (!keep) {
      (void)nyir_erase_instruction(f, i - 1);
      continue;
    }
    if (in->a >= 0)
      used[in->a] = true;
    if (in->b >= 0)
      used[in->b] = true;
    if (in->c >= 0)
      used[in->c] = true;
    if (in->d >= 0)
      used[in->d] = true;
    if (in->e >= 0)
      used[in->e] = true;
    if (in->f >= 0)
      used[in->f] = true;
    for (size_t k = 0; k < in->extra_args_len; ++k) {
      if (in->extra_args[k] >= 0)
        used[in->extra_args[k]] = true;
    }
    for (size_t k = 0; k < in->phi_incoming_len; ++k)
      if (in->phi_incoming[k].value >= 0)
        used[in->phi_incoming[k].value] = true;
  }
  free(label_referenced);
  free(used);
#undef NIR_LABEL_REFERENCED
  return true;
}
