/*
 * Jump threading through a branch-only block.
 *
 * A threaded edge skips the block, so its PHI edge is removed.  The selected
 * successor gains the predecessor directly; its incoming value is copied from
 * the skipped block only when that value is not defined in the skipped block.
 */
#include "code/ir/opt/util.h"

#include <stdlib.h>
#include <string.h>
#include <limits.h>

typedef struct {
  nyir_inst_t *phi;
  nyir_phi_incoming_t *incoming;
  size_t len;
} nyir_jump_phi_edit_t;

static int block_for_label(const nyir_cfg_t *cfg, int64_t label) {
  if (!cfg)
    return -1;
  for (size_t b = 0; b < cfg->block_count; ++b)
    if (cfg->block_label[b] == label)
      return b <= (size_t)INT_MAX ? (int)b : -1;
  return -1;
}

static size_t block_last(const nyir_func_t *f, const nyir_cfg_t *cfg,
                         size_t block) {
  size_t end = cfg->block_end[block];
  while (end > cfg->block_start[block] && f->data[end - 1].op == NYIR_NOP)
    --end;
  return end;
}

static bool branch_only(const nyir_func_t *f, const nyir_cfg_t *cfg,
                        size_t block, size_t *term_out) {
  size_t start = cfg->block_start[block];
  size_t end = block_last(f, cfg, block);
  if (end == start)
    return false;
  size_t term = end - 1;
  if (f->data[term].op != NYIR_BR_IF)
    return false;
  for (size_t i = start; i < term; ++i) {
    nyir_op_t op = f->data[i].op;
    if (op != NYIR_LABEL && op != NYIR_PHI && op != NYIR_NOP)
      return false;
  }
  if (term_out)
    *term_out = term;
  return true;
}

static int phi_incoming(const nyir_inst_t *phi, int64_t label) {
  for (size_t i = 0; i < phi->phi_incoming_len; ++i)
    if (phi->phi_incoming[i].predecessor_label == label)
      return i <= (size_t)INT_MAX ? (int)i : -1;
  return -1;
}

static bool value_constant(const nyir_func_t *f, const bool *known,
                           const int64_t *values, int value, int64_t *out) {
  if (value < 0 || value >= f->next_value || !known[value])
    return false;
  if (out)
    *out = values[value];
  return true;
}

static bool def_in_block(const nyir_cfg_t *cfg, const nyir_func_t *f,
                         const int *defs, int value, size_t block) {
  if (value < 0 || value >= f->next_value || defs[value] < 0)
    return false;
  return (size_t)defs[value] < f->len &&
         cfg->inst_block[(size_t)defs[value]] == block;
}

static void free_edits(nyir_jump_phi_edit_t *edits, size_t count) {
  if (!edits)
    return;
  for (size_t i = 0; i < count; ++i)
    free(edits[i].incoming);
  free(edits);
}

static bool thread_edge(nyir_func_t *f, const nyir_cfg_t *cfg, const int *defs,
                        size_t pred, size_t through, size_t target) {
  if (pred == through || through == target || pred == target ||
      cfg->block_label[through] < 0 || cfg->block_label[target] < 0)
    return false;
  size_t pstart = cfg->block_start[pred];
  size_t pend = block_last(f, cfg, pred);
  if (pend == pstart)
    return false;
  nyir_inst_t *pterm = &f->data[pend - 1];
  if ((pterm->op != NYIR_BR && pterm->op != NYIR_BR_IF) ||
      pterm->imm != cfg->block_label[through])
    return false;

  bool has_direct = false;
  for (size_t e = cfg->pred_offsets[target]; e < cfg->pred_offsets[target + 1];
       ++e)
    if (cfg->pred_blocks[e] == pred)
      has_direct = true;

  size_t phi_count = 0;
  for (size_t i = 0; i < f->len; ++i)
    if (f->data[i].op == NYIR_PHI)
      ++phi_count;
  nyir_jump_phi_edit_t *edits = phi_count ?
      calloc(phi_count, sizeof(*edits)) : NULL;
  if (phi_count && !edits)
    return false;
  size_t edit_count = 0;
  int64_t pred_label = cfg->block_label[pred];
  int64_t through_label = cfg->block_label[through];
  int64_t target_label = cfg->block_label[target];

  size_t bstart = cfg->block_start[through], bend = cfg->block_end[through];
  for (size_t i = bstart; i < bend; ++i) {
    nyir_inst_t *phi = &f->data[i];
    if (phi->op != NYIR_PHI)
      continue;
    int at = phi_incoming(phi, pred_label);
    if (at < 0 || cfg->pred_offsets[through + 1] -
                      cfg->pred_offsets[through] <= 1) {
      free_edits(edits, edit_count);
      return false;
    }
    nyir_jump_phi_edit_t *edit = &edits[edit_count++];
    edit->len = phi->phi_incoming_len - 1;
    edit->incoming = malloc(edit->len * sizeof(*edit->incoming));
    if (!edit->incoming) {
      free_edits(edits, edit_count);
      return false;
    }
    edit->phi = phi;
    memcpy(edit->incoming, phi->phi_incoming,
           (size_t)at * sizeof(*edit->incoming));
    memcpy(edit->incoming + at, phi->phi_incoming + at + 1,
           (edit->len - (size_t)at) * sizeof(*edit->incoming));
  }

  if (!has_direct) {
    for (size_t i = cfg->block_start[target]; i < cfg->block_end[target]; ++i) {
      nyir_inst_t *phi = &f->data[i];
      if (phi->op != NYIR_PHI)
        continue;
      int at = phi_incoming(phi, through_label);
      if (at < 0 || def_in_block(cfg, f, defs,
                                 phi->phi_incoming[at].value, through)) {
        free_edits(edits, edit_count);
        return false;
      }
      nyir_jump_phi_edit_t *edit = &edits[edit_count++];
      edit->len = phi->phi_incoming_len + 1;
      edit->incoming = malloc(edit->len * sizeof(*edit->incoming));
      if (!edit->incoming) {
        free_edits(edits, edit_count);
        return false;
      }
      edit->phi = phi;
      memcpy(edit->incoming, phi->phi_incoming,
             phi->phi_incoming_len * sizeof(*edit->incoming));
      edit->incoming[phi->phi_incoming_len] =
          (nyir_phi_incoming_t){.predecessor_label = pred_label,
                                .value = phi->phi_incoming[at].value};
    }
  }

  for (size_t i = 0; i < edit_count; ++i) {
    nyir_inst_t *phi = edits[i].phi;
    free(phi->phi_incoming);
    phi->phi_incoming = edits[i].incoming;
    phi->phi_incoming_len = edits[i].len;
    edits[i].incoming = NULL;
  }
  free_edits(edits, edit_count);
  pterm->imm = target_label;
  return true;
}

bool nyir_jump_thread(nyir_func_t *f) {
  if (!f)
    return true;
  bool *known = f->next_value > 0 ? calloc((size_t)f->next_value, sizeof(*known)) : NULL;
  int64_t *values = f->next_value > 0 ? calloc((size_t)f->next_value, sizeof(*values)) : NULL;
  int *defs = nyir_build_defs(f);
  if ((f->next_value > 0 && (!known || !values || !defs))) {
    free(known); free(values); free(defs);
    return false;
  }
  if (!nir_collect_consts(f, known, values)) {
    free(known); free(values); free(defs);
    return false;
  }

  for (;;) {
    nyir_cfg_t cfg = {0};
    if (!nyir_cfg_build_topology(f, &cfg)) {
      free(known); free(values); free(defs);
      return false;
    }
    bool threaded = false;
    for (size_t through = 0; through < cfg.block_count && !threaded; ++through) {
      size_t term_i = 0;
      if (!cfg.reachable[through] || !branch_only(f, &cfg, through, &term_i))
        continue;
      const nyir_inst_t *term = &f->data[term_i];
      int64_t cond_value = 0;
      bool direct_constant = value_constant(f, known, values, term->a,
                                             &cond_value);
      int cond_def = term->a >= 0 && term->a < f->next_value ? defs[term->a] : -1;
      const nyir_inst_t *cond_phi = cond_def >= 0 &&
          (size_t)cond_def < f->len && f->data[cond_def].op == NYIR_PHI
          ? &f->data[cond_def] : NULL;
      for (size_t pe = cfg.pred_offsets[through];
           pe < cfg.pred_offsets[through + 1] && !threaded; ++pe) {
        size_t pred = cfg.pred_blocks[pe];
        int64_t path_value = cond_value;
        bool path_constant = direct_constant;
        if (cond_phi) {
          int at = phi_incoming(cond_phi, cfg.block_label[pred]);
          path_constant = at >= 0 && value_constant(
              f, known, values, cond_phi->phi_incoming[at].value, &path_value);
        }
        if (!path_constant)
          continue;
        size_t target = SIZE_MAX;
        if (path_value != 0) {
          int block = block_for_label(&cfg, term->imm);
          if (block >= 0)
            target = (size_t)block;
        } else if (through + 1 < cfg.block_count) {
          target = through + 1;
        }
        if (target == SIZE_MAX || target == through || target == pred)
          continue;
        if (thread_edge(f, &cfg, defs, pred, through, target))
          threaded = true;
      }
    }
    nyir_cfg_free(&cfg);
    if (!threaded)
      break;
    free(defs);
    defs = nyir_build_defs(f);
    if (!defs)
      break;
  }
  free(known); free(values); free(defs);
  return true;
}
