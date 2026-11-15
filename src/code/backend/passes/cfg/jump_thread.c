/*
 * Jump threading through a branch-only block.
 *
 * A threaded edge skips the block, so its PHI edge is removed.  The selected
 * successor gains the predecessor directly; its incoming value is copied from
 * the skipped block only when that value is not defined in the skipped block.
 */
#include "base/debug.h"
#include "code/backend/passes/util.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static bool jump_thread_trace_enabled(void) {
  return ny_dbg_is_enabled(NY_DBG_HEURISTIC) ||
         ny_trace_enabled("NY_TRACE_JUMP_THREAD");
}

typedef struct {
  nyr_inst_t *phi;
  nyr_phi_incoming_t *incoming;
  size_t len;
} nyr_jump_phi_edit_t;

static int block_for_label(const nyr_cfg_t *cfg, int64_t label) {
  size_t b = 0;
  if (nyr_cfg_block_for_label(cfg, label, &b) && b <= (size_t)INT_MAX)
    return (int)b;
  return -1;
}

static bool branch_only(const nyr_func_t *f, const nyr_cfg_t *cfg, size_t block,
                        size_t *term_out) {
  size_t start = cfg->block_start[block];
  size_t end = nyr_block_last_non_nop(f, cfg, block);
  if (end == start)
    return false;
  size_t term = end - 1;
  if (f->data[term].op != NYIR_BR_IF)
    return false;
  for (size_t i = start; i < term; ++i) {
    nyr_op_t op = f->data[i].op;
    if (op != NYIR_LABEL && op != NYIR_PHI && op != NYIR_NOP)
      return false;
  }
  if (term_out)
    *term_out = term;
  return true;
}

static int phi_incoming(const nyr_inst_t *phi, int64_t label) {
  for (size_t i = 0; i < phi->phi_incoming_len; ++i)
    if (phi->phi_incoming[i].predecessor_label == label)
      return i <= (size_t)INT_MAX ? (int)i : -1;
  return -1;
}

static bool value_constant(const nyr_func_t *f, const bool *known,
                           const int64_t *values, int value, int64_t *out) {
  if (value < 0 || value >= f->next_value || !known[value])
    return false;
  if (out)
    *out = values[value];
  return true;
}

static bool def_in_block(const nyr_cfg_t *cfg, const nyr_func_t *f,
                         const int *defs, int value, size_t block) {
  if (value < 0 || value >= f->next_value || defs[value] < 0)
    return false;
  return (size_t)defs[value] < f->len &&
         cfg->inst_block[(size_t)defs[value]] == block;
}

/*
 * Bypassing a PHI block also bypasses the definition of each PHI result.
 * Retargeting successor PHIs is insufficient when the value is used directly
 * in the successor (or farther down the path), so only thread blocks whose
 * PHI results are local to that block.
 */
static bool has_live_out_phi(const nyr_func_t *f, const nyr_cfg_t *cfg,
                             const int *defs, size_t block) {
  bool has_phi = false;
  for (size_t i = cfg->block_start[block]; i < cfg->block_end[block]; ++i)
    has_phi |= f->data[i].op == NYIR_PHI && f->data[i].dst >= 0;
  if (!has_phi)
    return false;
  for (size_t use = 0; use < f->len; ++use) {
    if (cfg->inst_block[use] == block)
      continue;
    const nyr_inst_t *in = &f->data[use];
    const int operands[] = {in->a, in->b, in->c, in->d, in->e, in->f};
    for (size_t i = 0; i < sizeof(operands) / sizeof(*operands); ++i)
      if (def_in_block(cfg, f, defs, operands[i], block))
        return true;
    for (size_t i = 0; i < in->phi_incoming_len; ++i)
      if (def_in_block(cfg, f, defs, in->phi_incoming[i].value, block))
        return true;
    for (size_t i = 0; i < in->extra_args_len; ++i)
      if (def_in_block(cfg, f, defs, in->extra_args[i], block))
        return true;
  }
  return false;
}

static void free_edits(nyr_jump_phi_edit_t *edits, size_t count) {
  if (!edits)
    return;
  for (size_t i = 0; i < count; ++i)
    free(edits[i].incoming);
  free(edits);
}

static bool thread_edge(nyr_func_t *f, const nyr_cfg_t *cfg, const int *defs,
                        size_t pred, size_t through, size_t target) {
  if (pred == through || through == target || pred == target ||
      cfg->block_label[through] < 0 || cfg->block_label[target] < 0 ||
      has_live_out_phi(f, cfg, defs, through))
    return false;
  size_t pstart = cfg->block_start[pred];
  size_t pend = nyr_block_last_non_nop(f, cfg, pred);
  if (pend == pstart)
    return false;
  nyr_inst_t *pterm = &f->data[pend - 1];
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
  nyr_jump_phi_edit_t *edits =
      phi_count ? calloc(phi_count, sizeof(*edits)) : NULL;
  if (phi_count && !edits)
    return false;
  size_t edit_count = 0;
  int64_t pred_label = cfg->block_label[pred];
  int64_t through_label = cfg->block_label[through];
  int64_t target_label = cfg->block_label[target];

  size_t bstart = cfg->block_start[through], bend = cfg->block_end[through];
  for (size_t i = bstart; i < bend; ++i) {
    nyr_inst_t *phi = &f->data[i];
    if (phi->op != NYIR_PHI)
      continue;
    int at = phi_incoming(phi, pred_label);
    if (at < 0 ||
        cfg->pred_offsets[through + 1] - cfg->pred_offsets[through] <= 1) {
      free_edits(edits, edit_count);
      return false;
    }
    nyr_jump_phi_edit_t *edit = &edits[edit_count++];
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
      nyr_inst_t *phi = &f->data[i];
      if (phi->op != NYIR_PHI)
        continue;
      int at = phi_incoming(phi, through_label);
      if (at < 0 ||
          def_in_block(cfg, f, defs, phi->phi_incoming[at].value, through)) {
        free_edits(edits, edit_count);
        return false;
      }
      nyr_jump_phi_edit_t *edit = &edits[edit_count++];
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
          (nyr_phi_incoming_t){.predecessor_label = pred_label,
                               .value = phi->phi_incoming[at].value};
    }
  }

  for (size_t i = 0; i < edit_count; ++i) {
    nyr_inst_t *phi = edits[i].phi;
    free(phi->phi_incoming);
    phi->phi_incoming = edits[i].incoming;
    phi->phi_incoming_len = edits[i].len;
    edits[i].incoming = NULL;
  }
  free_edits(edits, edit_count);
  pterm->imm = target_label;
  return true;
}

bool nyr_jump_thread(nyr_func_t *f) {
  if (!f || f->next_value <= 0)
    return true;
  bool *known =
      f->next_value > 0 ? calloc((size_t)f->next_value, sizeof(*known)) : NULL;
  int64_t *values =
      f->next_value > 0 ? calloc((size_t)f->next_value, sizeof(*values)) : NULL;
  int *defs = nyr_build_defs(f);
  if ((f->next_value > 0 && (!known || !values || !defs))) {
    free(known);
    free(values);
    free(defs);
    return false;
  }
  if (!nir_collect_consts(f, known, values)) {
    free(known);
    free(values);
    free(defs);
    return false;
  }

  for (;;) {
    nyr_cfg_t cfg = {0};
    if (!nyr_cfg_build_topology(f, &cfg)) {
      free(known);
      free(values);
      free(defs);
      return false;
    }
    bool threaded = false;
    for (size_t through = 0; through < cfg.block_count && !threaded;
         ++through) {
      size_t term_i = 0;
      if (!cfg.reachable[through] || !branch_only(f, &cfg, through, &term_i))
        continue;
      const nyr_inst_t *term = &f->data[term_i];
      int64_t cond_value = 0;
      bool direct_constant =
          value_constant(f, known, values, term->a, &cond_value);
      int cond_def =
          term->a >= 0 && term->a < f->next_value ? defs[term->a] : -1;
      const nyr_inst_t *cond_phi = cond_def >= 0 && (size_t)cond_def < f->len &&
                                           f->data[cond_def].op == NYIR_PHI
                                       ? &f->data[cond_def]
                                       : NULL;
      for (size_t pe = cfg.pred_offsets[through];
           pe < cfg.pred_offsets[through + 1] && !threaded; ++pe) {
        size_t pred = cfg.pred_blocks[pe];
        int64_t path_value = cond_value;
        bool path_constant = direct_constant;
        if (cond_phi) {
          int at = phi_incoming(cond_phi, cfg.block_label[pred]);
          path_constant =
              at >= 0 &&
              value_constant(f, known, values, cond_phi->phi_incoming[at].value,
                             &path_value);
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
          /*
           * The false edge often falls into an unlabeled block holding only
           * a branch. Follow it, since an unlabeled block cannot be targeted.
           */
          if (cfg.block_label[target] < 0) {
            size_t last = nyr_block_last_non_nop(f, &cfg, target);
            if (last == cfg.block_start[target] + 1 &&
                f->data[last - 1].op == NYIR_BR) {
              int block = block_for_label(&cfg, f->data[last - 1].imm);
              target = block >= 0 ? (size_t)block : SIZE_MAX;
            }
          }
        }
        if (target == SIZE_MAX || target == through || target == pred)
          continue;
        if (thread_edge(f, &cfg, defs, pred, through, target)) {
          if (jump_thread_trace_enabled())
            ny_dbg_heuristic_event(
                "JUMP_THREAD",
                "accept edge b%zu -> b%zu -> b%zu: predecessor-specific "
                "constant %lld selects the branch-only block's target; PHI "
                "incoming edges were updated",
                pred, through, target, (long long)path_value);
          threaded = true;
        } else if (jump_thread_trace_enabled()) {
          ny_dbg_heuristic_event(
              "JUMP_THREAD",
              "reject edge b%zu -> b%zu -> b%zu: PHI predecessor/value "
              "updates are not valid for this path",
              pred, through, target);
        }
      }
    }
    nyr_cfg_free(&cfg);
    if (!threaded)
      break;
    free(defs);
    defs = nyr_build_defs(f);
    if (!defs)
      break;
  }
  free(known);
  free(values);
  free(defs);
  return true;
}

/*
 * A conditional branch over an unconditional one
 *
 *     br.if c -> L1;  br -> L2;  L1:
 *
 * costs an extra jump on the path to L2 and a taken branch on the path to L1.
 * Inverting the comparison sends the first path to L2 and lets L1 fall through.
 * Float comparisons stay as they are: inverting one changes its NaN result.
 */
bool nyr_invert_fallthrough_branches(nyr_func_t *f) {
  if (!f || f->next_value <= 0 || !f->len)
    return true;
  size_t nv = (size_t)f->next_value;
  unsigned *uses = calloc(nv, sizeof(*uses));
  int *defs = nyr_build_defs(f);
  if (!uses || !defs) {
    free(uses);
    free(defs);
    return false;
  }
#define NOTE_USE(v)                   \
  do {                                \
    if ((v) >= 0 && (size_t)(v) < nv) \
      ++uses[(v)];                    \
  } while (0)
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    NOTE_USE(in->a);
    NOTE_USE(in->b);
    NOTE_USE(in->c);
    NOTE_USE(in->d);
    NOTE_USE(in->e);
    NOTE_USE(in->f);
    for (size_t k = 0; k < in->phi_incoming_len; ++k)
      NOTE_USE(in->phi_incoming[k].value);
    for (size_t k = 0; in->extra_args && k < in->extra_args_len; ++k)
      NOTE_USE(in->extra_args[k]);
  }
#undef NOTE_USE
  /*
   * Loop headers keep their shape: machine lowering rotates them, which
   * replaces the backedge and the guard with one conditional branch.
   */
  int64_t max_label = -1;
  for (size_t i = 0; i < f->len; ++i)
    if (f->data[i].op == NYIR_LABEL && f->data[i].imm > max_label)
      max_label = f->data[i].imm;
  size_t *last_jump =
      max_label >= 0 ? calloc((size_t)max_label + 1, sizeof(*last_jump)) : NULL;
  if (max_label >= 0 && !last_jump) {
    free(uses);
    free(defs);
    return false;
  }
  for (size_t i = 0; i < f->len; ++i)
    if ((f->data[i].op == NYIR_BR || f->data[i].op == NYIR_BR_IF) &&
        f->data[i].imm >= 0 && f->data[i].imm <= max_label)
      last_jump[f->data[i].imm] = i + 1;
  int64_t block_label = -1;
  for (size_t i = 0; i < f->len; ++i) {
    nyr_inst_t *branch = &f->data[i];
    if (branch->op == NYIR_LABEL)
      block_label = branch->imm;
    if (branch->op != NYIR_BR_IF || branch->a < 0 || (size_t)branch->a >= nv ||
        defs[branch->a] < 0 || uses[branch->a] != 1)
      continue;
    if (block_label >= 0 && last_jump[block_label] > i + 1)
      continue;
    size_t jump_at = i + 1;
    while (jump_at < f->len && f->data[jump_at].op == NYIR_NOP)
      ++jump_at;
    if (jump_at >= f->len || f->data[jump_at].op != NYIR_BR)
      continue;
    size_t label_at = jump_at + 1;
    while (label_at < f->len && f->data[label_at].op == NYIR_NOP)
      ++label_at;
    if (label_at >= f->len || f->data[label_at].op != NYIR_LABEL ||
        f->data[label_at].imm != branch->imm)
      continue;
    nyr_inst_t *cmp = &f->data[defs[branch->a]];
    if (cmp->op != NYIR_CMP_I64)
      continue;
    int inverted;
    switch (cmp->cmp) {
    case NYIR_CMP_EQ:
      inverted = NYIR_CMP_NE;
      break;
    case NYIR_CMP_NE:
      inverted = NYIR_CMP_EQ;
      break;
    case NYIR_CMP_LT:
      inverted = NYIR_CMP_GE;
      break;
    case NYIR_CMP_GE:
      inverted = NYIR_CMP_LT;
      break;
    case NYIR_CMP_GT:
      inverted = NYIR_CMP_LE;
      break;
    case NYIR_CMP_LE:
      inverted = NYIR_CMP_GT;
      break;
    default:
      continue;
    }
    cmp->cmp = inverted;
    branch->imm = f->data[jump_at].imm;
    nyr_inst_discard(&f->data[jump_at]);
  }
  free(last_jump);
  free(uses);
  free(defs);
  return true;
}
