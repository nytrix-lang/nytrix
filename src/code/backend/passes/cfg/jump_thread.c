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

static int64_t jt_eval_cmp(nyr_cmp_t cmp, int64_t a, int64_t b) {
  switch (cmp) {
  case NYIR_CMP_EQ:
    return a == b ? 1 : 0;
  case NYIR_CMP_NE:
    return a != b ? 1 : 0;
  case NYIR_CMP_LT:
    return a < b ? 1 : 0;
  case NYIR_CMP_LE:
    return a <= b ? 1 : 0;
  case NYIR_CMP_GT:
    return a > b ? 1 : 0;
  case NYIR_CMP_GE:
    return a >= b ? 1 : 0;
  default:
    return 0;
  }
}

static bool jt_is_pure_branch_op(nyr_op_t op) {
  switch (op) {
  case NYIR_LABEL:
  case NYIR_PHI:
  case NYIR_NOP:
  case NYIR_COPY:
  case NYIR_CONST_I64:
  case NYIR_ADD_I64:
  case NYIR_SUB_I64:
  case NYIR_MUL_I64:
  case NYIR_AND_I64:
  case NYIR_OR_I64:
  case NYIR_XOR_I64:
  case NYIR_SHL_I64:
  case NYIR_SAR_I64:
  case NYIR_CMP_I64:
  case NYIR_SELECT_I64:
    return true;
  default:
    return false;
  }
}

typedef struct {
  const nyr_inst_t *cond_phi;
  bool has_cmp;
  nyr_cmp_t cmp_op;
  int64_t cmp_const;
  bool phi_on_right;
  /* -1 = any evaluated result determines branch; 0 = only result==0 forces false; 1 = only result!=0 forces true */
  int forced_polarity;
} jt_cond_term_t;

static void jt_collect_cond_terms(const nyr_func_t *f, const nyr_cfg_t *cfg,
                                  const int *defs, const bool *known,
                                  const int64_t *values, size_t through, int v,
                                  int forced_polarity, unsigned depth,
                                  jt_cond_term_t *terms, size_t *count,
                                  size_t cap) {
  if (depth > 8 || *count >= cap || v < 0 || v >= f->next_value)
    return;
  if (defs[v] < 0 || (size_t)defs[v] >= f->len ||
      cfg->inst_block[(size_t)defs[v]] != through)
    return;
  const nyr_inst_t *in = &f->data[(size_t)defs[v]];
  if (in->op == NYIR_COPY) {
    jt_collect_cond_terms(f, cfg, defs, known, values, through, in->a,
                          forced_polarity, depth + 1, terms, count, cap);
    return;
  }
  if (in->op == NYIR_AND_I64 &&
      (forced_polarity == -1 || forced_polarity == 0)) {
    jt_collect_cond_terms(f, cfg, defs, known, values, through, in->a, 0,
                          depth + 1, terms, count, cap);
    jt_collect_cond_terms(f, cfg, defs, known, values, through, in->b, 0,
                          depth + 1, terms, count, cap);
    return;
  }
  if (in->op == NYIR_OR_I64 &&
      (forced_polarity == -1 || forced_polarity == 1)) {
    jt_collect_cond_terms(f, cfg, defs, known, values, through, in->a, 1,
                          depth + 1, terms, count, cap);
    jt_collect_cond_terms(f, cfg, defs, known, values, through, in->b, 1,
                          depth + 1, terms, count, cap);
    return;
  }
  if (in->op == NYIR_PHI) {
    terms[(*count)++] = (jt_cond_term_t){
        .cond_phi = in,
        .has_cmp = false,
        .cmp_op = NYIR_CMP_NE,
        .cmp_const = 0,
        .phi_on_right = false,
        .forced_polarity = forced_polarity,
    };
    return;
  }
  if (in->op == NYIR_CMP_I64) {
    int la = in->a, lb = in->b;
    for (unsigned d = 0; d < 8 && la >= 0 && la < f->next_value && defs[la] >= 0 &&
                         cfg->inst_block[(size_t)defs[la]] == through &&
                         f->data[(size_t)defs[la]].op == NYIR_COPY;
         ++d)
      la = f->data[(size_t)defs[la]].a;
    for (unsigned d = 0; d < 8 && lb >= 0 && lb < f->next_value && defs[lb] >= 0 &&
                         cfg->inst_block[(size_t)defs[lb]] == through &&
                         f->data[(size_t)defs[lb]].op == NYIR_COPY;
         ++d)
      lb = f->data[(size_t)defs[lb]].a;
    int64_t ca = 0, cb = 0;
    bool ka = (la >= 0 && la < f->next_value && known[la]);
    bool kb = (lb >= 0 && lb < f->next_value && known[lb]);
    if (ka)
      ca = values[la];
    if (kb)
      cb = values[lb];
    if (kb && la >= 0 && la < f->next_value && defs[la] >= 0 &&
        cfg->inst_block[(size_t)defs[la]] == through &&
        f->data[(size_t)defs[la]].op == NYIR_PHI) {
      terms[(*count)++] = (jt_cond_term_t){
          .cond_phi = &f->data[(size_t)defs[la]],
          .has_cmp = true,
          .cmp_op = in->cmp,
          .cmp_const = cb,
          .phi_on_right = false,
          .forced_polarity = forced_polarity,
      };
    } else if (ka && lb >= 0 && lb < f->next_value && defs[lb] >= 0 &&
               cfg->inst_block[(size_t)defs[lb]] == through &&
               f->data[(size_t)defs[lb]].op == NYIR_PHI) {
      terms[(*count)++] = (jt_cond_term_t){
          .cond_phi = &f->data[(size_t)defs[lb]],
          .has_cmp = true,
          .cmp_op = in->cmp,
          .cmp_const = ca,
          .phi_on_right = true,
          .forced_polarity = forced_polarity,
      };
    }
  }
}

static bool jt_block_is_branch_or_cmp(const nyr_func_t *f, const nyr_cfg_t *cfg,
                                      size_t block, size_t *term_out,
                                      size_t *cmp_out, bool *pure_tree_out) {
  size_t start = cfg->block_start[block];
  size_t end = nyr_block_last_non_nop(f, cfg, block);
  if (end == start)
    return false;
  size_t term = end - 1;
  if (f->data[term].op == NYIR_BR && term > start &&
      f->data[term - 1].op == NYIR_BR_IF)
    --term;
  if (f->data[term].op != NYIR_BR_IF)
    return false;
  size_t cmp_idx = SIZE_MAX;
  bool simple = true;
  bool has_bool_tree = false;
  for (size_t i = start; i < term; ++i) {
    nyr_op_t op = f->data[i].op;
    if (op == NYIR_LABEL || op == NYIR_PHI || op == NYIR_NOP || op == NYIR_COPY)
      continue;
    if (op == NYIR_CMP_I64 && cmp_idx == SIZE_MAX &&
        f->data[i].dst == f->data[term].a) {
      cmp_idx = i;
      continue;
    }
    simple = false;
    if (!jt_is_pure_branch_op(op))
      return false;
    if ((op == NYIR_AND_I64 || op == NYIR_OR_I64) &&
        f->data[i].dst == f->data[term].a)
      has_bool_tree = true;
  }
  if (!simple && !has_bool_tree)
    return false;
  if (term_out)
    *term_out = term;
  if (cmp_out)
    *cmp_out = simple ? cmp_idx : SIZE_MAX;
  if (pure_tree_out)
    *pure_tree_out = !simple && has_bool_tree;
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

static int jt_strip_local_copies(const nyr_func_t *f, const nyr_cfg_t *cfg,
                                 const int *defs, size_t block, int v) {
  for (unsigned depth = 0; depth < 8; ++depth) {
    if (!def_in_block(cfg, f, defs, v, block))
      break;
    const nyr_inst_t *in = &f->data[(size_t)defs[v]];
    if (in->op != NYIR_COPY)
      break;
    v = in->a;
  }
  return v;
}

static bool jt_block_is_phi_only_bridge(const nyr_func_t *f,
                                        const nyr_cfg_t *cfg, size_t block) {
  if (block >= cfg->block_count)
    return false;
  if (cfg->succ_offsets[block + 1] - cfg->succ_offsets[block] != 1)
    return false;
  for (size_t i = cfg->block_start[block]; i < cfg->block_end[block]; ++i) {
    nyr_op_t op = f->data[i].op;
    if (op != NYIR_LABEL && op != NYIR_PHI && op != NYIR_NOP && op != NYIR_BR)
      return false;
  }
  return true;
}

static bool jt_block_is_empty_bridge(const nyr_func_t *f, const nyr_cfg_t *cfg,
                                     size_t block) {
  if (block >= cfg->block_count)
    return false;
  if (cfg->succ_offsets[block + 1] - cfg->succ_offsets[block] != 1)
    return false;
  for (size_t i = cfg->block_start[block]; i < cfg->block_end[block]; ++i) {
    nyr_op_t op = f->data[i].op;
    if (op != NYIR_LABEL && op != NYIR_NOP && op != NYIR_BR)
      return false;
  }
  return true;
}

/*
 * Check if bypassing `through` via `src -> ... -> through -> target` preserves
 * SSA dominance and values for every definition inside `through` (and `mid`,
 * when threading across a PHI-only forwarding block like L19 -> L16 -> L11).
 */
static bool jt_live_out_safe(const nyr_func_t *f, const nyr_cfg_t *cfg,
                             const int *defs, size_t src, size_t pred,
                             size_t mid, size_t through, size_t target) {
  bool *reach = calloc(cfg->block_count, sizeof(*reach));
  size_t *queue = malloc(cfg->block_count * sizeof(*queue));
  if (!reach || !queue) {
    free(reach);
    free(queue);
    return false;
  }
  size_t qh = 0, qt = 0;
  reach[target] = true;
  queue[qt++] = target;
  while (qh < qt) {
    size_t b = queue[qh++];
    for (size_t e = cfg->succ_offsets[b]; e < cfg->succ_offsets[b + 1]; ++e) {
      size_t s = cfg->succ_blocks[e];
      if (s < cfg->block_count && s != through && !reach[s]) {
        reach[s] = true;
        queue[qt++] = s;
      }
    }
  }
  free(queue);

  bool dominates_src = nyr_cfg_dominates(cfg, through, src);
  bool safe = true;
  for (size_t use = 0; use < f->len && safe; ++use) {
    size_t ub = cfg->inst_block[use];
    if (ub == through)
      continue;
    const nyr_inst_t *in = &f->data[use];
    const int operands[] = {in->a, in->b, in->c, in->d, in->e, in->f};
    for (size_t k = 0; k < sizeof(operands) / sizeof(*operands) && safe; ++k) {
      int v = operands[k];
      if (mid != SIZE_MAX && def_in_block(cfg, f, defs, v, mid)) {
        safe = false;
        break;
      }
      if (!def_in_block(cfg, f, defs, v, through))
        continue;
      const nyr_inst_t *def = &f->data[(size_t)defs[v]];
      if (def->op != NYIR_PHI) {
        safe = false;
        break;
      }
      if (!dominates_src) {
        safe = false;
        break;
      }
      if (reach[ub]) {
        int at = phi_incoming(def, cfg->block_label[pred]);
        if (at < 0) {
          safe = false;
          break;
        }
        int iv = def->phi_incoming[at].value;
        if (mid != SIZE_MAX && def_in_block(cfg, f, defs, iv, mid)) {
          const nyr_inst_t *mdef = &f->data[(size_t)defs[iv]];
          if (mdef->op != NYIR_PHI) {
            safe = false;
            break;
          }
          int mat = phi_incoming(mdef, cfg->block_label[src]);
          if (mat < 0) {
            safe = false;
            break;
          }
          iv = mdef->phi_incoming[mat].value;
        }
        if (iv != v)
          safe = false;
      }
    }
    for (size_t k = 0; k < in->phi_incoming_len && safe; ++k) {
      int v = in->phi_incoming[k].value;
      if (mid != SIZE_MAX && ub != through &&
          def_in_block(cfg, f, defs, v, mid)) {
        safe = false;
        break;
      }
      if (!def_in_block(cfg, f, defs, v, through))
        continue;
      const nyr_inst_t *def = &f->data[(size_t)defs[v]];
      if (def->op != NYIR_PHI || !dominates_src) {
        safe = false;
        break;
      }
      int pb = block_for_label(cfg, in->phi_incoming[k].predecessor_label);
      if (pb >= 0 && reach[(size_t)pb]) {
        int at = phi_incoming(def, cfg->block_label[pred]);
        if (at < 0) {
          safe = false;
          break;
        }
        int iv = def->phi_incoming[at].value;
        if (mid != SIZE_MAX && def_in_block(cfg, f, defs, iv, mid)) {
          const nyr_inst_t *mdef = &f->data[(size_t)defs[iv]];
          if (mdef->op != NYIR_PHI) {
            safe = false;
            break;
          }
          int mat = phi_incoming(mdef, cfg->block_label[src]);
          if (mat < 0) {
            safe = false;
            break;
          }
          iv = mdef->phi_incoming[mat].value;
        }
        if (iv != v)
          safe = false;
      }
    }
    for (size_t k = 0; k < in->extra_args_len && safe; ++k) {
      int v = in->extra_args[k];
      if ((mid != SIZE_MAX && def_in_block(cfg, f, defs, v, mid)) ||
          def_in_block(cfg, f, defs, v, through))
        safe = false;
    }
  }
  free(reach);
  return safe;
}

static void free_edits(nyr_jump_phi_edit_t *edits, size_t count) {
  if (!edits)
    return;
  for (size_t i = 0; i < count; ++i)
    free(edits[i].incoming);
  free(edits);
}

static void jt_replace_value_uses(nyr_func_t *f, int old_v, int new_v) {
  if (!f || old_v < 0 || new_v < 0 || old_v == new_v)
    return;
  for (size_t i = 0; i < f->len; ++i) {
    nyr_inst_t *in = &f->data[i];
    if (in->a == old_v)
      in->a = new_v;
    if (in->b == old_v)
      in->b = new_v;
    if (in->c == old_v)
      in->c = new_v;
    if (in->d == old_v)
      in->d = new_v;
    if (in->e == old_v)
      in->e = new_v;
    if (in->f == old_v)
      in->f = new_v;
    for (size_t k = 0; k < in->phi_incoming_len; ++k)
      if (in->phi_incoming[k].value == old_v)
        in->phi_incoming[k].value = new_v;
    for (size_t k = 0; in->extra_args && k < in->extra_args_len; ++k)
      if (in->extra_args[k] == old_v)
        in->extra_args[k] = new_v;
  }
}

static bool thread_edge_ex(nyr_func_t *f, const nyr_cfg_t *cfg, const int *defs,
                           size_t src, size_t pred, size_t mid, size_t through,
                           size_t target) {
  size_t phi_owner = (mid != SIZE_MAX) ? mid : through;
  if (src == through || through == target || src == target ||
      src == phi_owner || phi_owner == target ||
      cfg->block_label[through] < 0 || cfg->block_label[target] < 0 ||
      cfg->block_label[phi_owner] < 0 || cfg->block_label[src] < 0)
    return false;
  if (!jt_live_out_safe(f, cfg, defs, src, pred, mid, through, target))
    return false;
  size_t pstart = cfg->block_start[src];
  size_t pend = nyr_block_last_non_nop(f, cfg, src);
  if (pend == pstart)
    return false;
  nyr_inst_t *pterm = &f->data[pend - 1];
  int64_t old_target_label = cfg->block_label[phi_owner];
  bool src_fallthrough = false;
  if ((pterm->op == NYIR_BR || pterm->op == NYIR_BR_IF)) {
    if (pterm->imm != old_target_label)
      return false;
  } else {
    if (src + 1 != phi_owner ||
        cfg->succ_offsets[src + 1] - cfg->succ_offsets[src] != 1 ||
        cfg->succ_blocks[cfg->succ_offsets[src]] != phi_owner)
      return false;
    src_fallthrough = true;
  }

  bool has_direct = false;
  for (size_t e = cfg->pred_offsets[target]; e < cfg->pred_offsets[target + 1];
       ++e)
    if (cfg->pred_blocks[e] == src)
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
  int64_t src_label = cfg->block_label[src];
  int64_t through_label = cfg->block_label[through];
  int64_t target_label = cfg->block_label[target];

  size_t bstart = cfg->block_start[phi_owner], bend = cfg->block_end[phi_owner];
  for (size_t i = bstart; i < bend; ++i) {
    nyr_inst_t *phi = &f->data[i];
    if (phi->op != NYIR_PHI)
      continue;
    int at = phi_incoming(phi, src_label);
    if (at < 0 ||
        cfg->pred_offsets[phi_owner + 1] - cfg->pred_offsets[phi_owner] <= 1) {
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
      if (at < 0) {
        free_edits(edits, edit_count);
        return false;
      }
      int val = phi->phi_incoming[at].value;
      if (def_in_block(cfg, f, defs, val, through)) {
        const nyr_inst_t *tdef = &f->data[(size_t)defs[val]];
        if (tdef->op != NYIR_PHI || !nyr_cfg_dominates(cfg, through, src)) {
          free_edits(edits, edit_count);
          return false;
        }
        int tat = phi_incoming(tdef, cfg->block_label[pred]);
        if (tat < 0) {
          free_edits(edits, edit_count);
          return false;
        }
        int iv = tdef->phi_incoming[tat].value;
        if (mid != SIZE_MAX && def_in_block(cfg, f, defs, iv, mid)) {
          const nyr_inst_t *mdef = &f->data[(size_t)defs[iv]];
          int mat = mdef->op == NYIR_PHI ? phi_incoming(mdef, src_label) : -1;
          if (mat < 0) {
            free_edits(edits, edit_count);
            return false;
          }
          iv = mdef->phi_incoming[mat].value;
        }
        if (iv != val) {
          free_edits(edits, edit_count);
          return false;
        }
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
          (nyr_phi_incoming_t){.predecessor_label = src_label, .value = val};
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
  if (!src_fallthrough)
    pterm->imm = target_label;
  /*
   * Collapse any 1-incoming PHIs left in `phi_owner` so nested PHI-only merge
   * blocks (e.g. `if c1 { go = 0 } else { if c2 { go = 0 } else { ... } }`)
   * turn immediately into empty bridges for the next threading iteration.
   */
  for (size_t i = bstart; i < bend; ++i) {
    nyr_inst_t *phi = &f->data[i];
    if (phi->op != NYIR_PHI || phi->phi_incoming_len != 1 || phi->dst < 0)
      continue;
    int sole = phi->phi_incoming[0].value;
    if (sole >= 0 && sole != phi->dst) {
      int old_dst = phi->dst;
      nyr_inst_discard(phi);
      jt_replace_value_uses(f, old_dst, sole);
    }
  }
  if (src_fallthrough) {
    size_t ins_at = cfg->block_end[src];
    if (!nir_ensure_inst_space(f, 1))
      return false;
    memmove(&f->data[ins_at + 1], &f->data[ins_at],
            (f->len - ins_at) * sizeof(*f->data));
    f->len += 1;
    f->data[ins_at] = (nyr_inst_t){
        .op = NYIR_BR,
        .dst = -1,
        .a = -1,
        .b = -1,
        .c = -1,
        .d = -1,
        .e = -1,
        .f = -1,
        .imm = target_label,
    };
    f->data[ins_at].effects = nyr_inst_effects(&f->data[ins_at]);
    nyr_refresh_metadata(f);
  }
  return true;
}

static size_t jt_pick_target(const nyr_func_t *f, const nyr_cfg_t *cfg,
                             size_t through, size_t term_i,
                             int64_t path_value) {
  const nyr_inst_t *term = &f->data[term_i];
  if (path_value != 0) {
    int block = block_for_label(cfg, term->imm);
    return block >= 0 ? (size_t)block : SIZE_MAX;
  }
  /* Check for explicit trailing BR right after BR_IF in the same block. */
  size_t bend = nyr_block_last_non_nop(f, cfg, through);
  if (term_i + 1 < bend && f->data[bend - 1].op == NYIR_BR) {
    int block = block_for_label(cfg, f->data[bend - 1].imm);
    return block >= 0 ? (size_t)block : SIZE_MAX;
  }
  if (through + 1 < cfg->block_count) {
    size_t target = through + 1;
    if (cfg->block_label[target] < 0) {
      size_t last = nyr_block_last_non_nop(f, cfg, target);
      if (last == cfg->block_start[target] + 1 &&
          f->data[last - 1].op == NYIR_BR) {
        int block = block_for_label(cfg, f->data[last - 1].imm);
        target = block >= 0 ? (size_t)block : SIZE_MAX;
      }
    }
    return target;
  }
  return SIZE_MAX;
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
    if (!nyr_cfg_build(f, &cfg)) {
      free(known);
      free(values);
      free(defs);
      return false;
    }
    bool threaded = false;
    for (size_t through = 0; through < cfg.block_count && !threaded;
         ++through) {
      size_t term_i = 0, cmp_i = SIZE_MAX;
      bool pure_tree = false;
      if (!cfg.reachable[through] ||
          !jt_block_is_branch_or_cmp(f, &cfg, through, &term_i, &cmp_i,
                                     &pure_tree))
        continue;
      const nyr_inst_t *term = &f->data[term_i];
      int64_t cond_value = 0;
      bool direct_constant = false;
      jt_cond_term_t terms[8];
      size_t term_count = 0;
      if (pure_tree) {
        jt_collect_cond_terms(f, &cfg, defs, known, values, through, term->a,
                              -1, 0, terms, &term_count, 8);
        if (term_count == 0)
          continue;
      } else if (cmp_i == SIZE_MAX) {
        int cv = jt_strip_local_copies(f, &cfg, defs, through, term->a);
        direct_constant = value_constant(f, known, values, cv, &cond_value);
        if (def_in_block(&cfg, f, defs, cv, through) &&
            f->data[(size_t)defs[cv]].op == NYIR_PHI) {
          terms[term_count++] = (jt_cond_term_t){
              .cond_phi = &f->data[(size_t)defs[cv]],
              .has_cmp = false,
              .cmp_op = NYIR_CMP_NE,
              .cmp_const = 0,
              .phi_on_right = false,
              .forced_polarity = -1,
          };
        }
      } else {
        const nyr_inst_t *cmp = &f->data[cmp_i];
        int la = jt_strip_local_copies(f, &cfg, defs, through, cmp->a);
        int lb = jt_strip_local_copies(f, &cfg, defs, through, cmp->b);
        int64_t ca = 0, cb = 0;
        bool ka = value_constant(f, known, values, la, &ca);
        bool kb = value_constant(f, known, values, lb, &cb);
        if (ka && kb) {
          direct_constant = true;
          cond_value = jt_eval_cmp(cmp->cmp, ca, cb);
        } else if (kb && def_in_block(&cfg, f, defs, la, through) &&
                   f->data[(size_t)defs[la]].op == NYIR_PHI) {
          terms[term_count++] = (jt_cond_term_t){
              .cond_phi = &f->data[(size_t)defs[la]],
              .has_cmp = true,
              .cmp_op = cmp->cmp,
              .cmp_const = cb,
              .phi_on_right = false,
              .forced_polarity = -1,
          };
        } else if (ka && def_in_block(&cfg, f, defs, lb, through) &&
                   f->data[(size_t)defs[lb]].op == NYIR_PHI) {
          terms[term_count++] = (jt_cond_term_t){
              .cond_phi = &f->data[(size_t)defs[lb]],
              .has_cmp = true,
              .cmp_op = cmp->cmp,
              .cmp_const = ca,
              .phi_on_right = true,
              .forced_polarity = -1,
          };
        } else {
          continue;
        }
      }
      for (size_t pe = cfg.pred_offsets[through];
           pe < cfg.pred_offsets[through + 1] && !threaded; ++pe) {
        size_t pred = cfg.pred_blocks[pe];
        if (direct_constant) {
          size_t target = jt_pick_target(f, &cfg, through, term_i, cond_value);
          if (target != SIZE_MAX && target != through && target != pred &&
              thread_edge_ex(f, &cfg, defs, pred, pred, SIZE_MAX, through,
                             target)) {
            if (jump_thread_trace_enabled())
              ny_dbg_heuristic_event(
                  "JUMP_THREAD",
                  "accept edge b%zu -> b%zu -> b%zu: predecessor-specific "
                  "constant %lld selects target",
                  pred, through, target, (long long)cond_value);
            threaded = true;
          }
          continue;
        }
        for (size_t ti = 0; ti < term_count && !threaded; ++ti) {
          const jt_cond_term_t *ct = &terms[ti];
          const nyr_inst_t *cond_phi = ct->cond_phi;
          int at = phi_incoming(cond_phi, cfg.block_label[pred]);
          if (at < 0)
            continue;
          int inc_val = cond_phi->phi_incoming[at].value;
          int64_t pv = 0;
          if (value_constant(f, known, values, inc_val, &pv)) {
            int64_t path_value =
                ct->has_cmp
                    ? (ct->phi_on_right
                           ? jt_eval_cmp(ct->cmp_op, ct->cmp_const, pv)
                           : jt_eval_cmp(ct->cmp_op, pv, ct->cmp_const))
                    : pv;
            if ((ct->forced_polarity == 0 && path_value != 0) ||
                (ct->forced_polarity == 1 && path_value == 0))
              continue;
            size_t target =
                jt_pick_target(f, &cfg, through, term_i, path_value);
            if (target != SIZE_MAX && target != through && target != pred &&
                thread_edge_ex(f, &cfg, defs, pred, pred, SIZE_MAX, through,
                               target)) {
              if (jump_thread_trace_enabled())
                ny_dbg_heuristic_event(
                    "JUMP_THREAD",
                    "accept edge b%zu -> b%zu -> b%zu: predecessor-specific "
                    "constant %lld selects target",
                    pred, through, target, (long long)path_value);
              threaded = true;
            }
            continue;
          }
          /*
           * 2-hop PHI forwarding: `src -> mid (PHI-only) -> [empty bridge] ->
           * through -> target`. Resolves `while go == 1` exit arms that join at
           * a latch merge block before jumping back to the loop header.
           */
          if (inc_val < 0 || inc_val >= f->next_value || defs[inc_val] < 0)
            continue;
          size_t mid = cfg.inst_block[(size_t)defs[inc_val]];
          if (f->data[(size_t)defs[inc_val]].op != NYIR_PHI ||
              !jt_block_is_phi_only_bridge(f, &cfg, mid))
            continue;
          /* Verify mid reaches pred exclusively through empty bridges. */
          size_t walk = mid;
          bool reaches_pred = (walk == pred);
          for (unsigned hops = 0; !reaches_pred && hops < 4; ++hops) {
            if (cfg.succ_offsets[walk + 1] - cfg.succ_offsets[walk] != 1)
              break;
            size_t nx = cfg.succ_blocks[cfg.succ_offsets[walk]];
            if (nx == pred) {
              reaches_pred = jt_block_is_empty_bridge(f, &cfg, pred) &&
                             (cfg.pred_offsets[pred + 1] -
                                  cfg.pred_offsets[pred] ==
                              1);
              break;
            }
            if (!jt_block_is_empty_bridge(f, &cfg, nx) ||
                cfg.pred_offsets[nx + 1] - cfg.pred_offsets[nx] != 1)
              break;
            walk = nx;
          }
          if (!reaches_pred)
            continue;
          const nyr_inst_t *mphi = &f->data[(size_t)defs[inc_val]];
          for (size_t mk = 0; mk < mphi->phi_incoming_len && !threaded; ++mk) {
            int64_t mpv = 0;
            if (!value_constant(f, known, values, mphi->phi_incoming[mk].value,
                                &mpv))
              continue;
            int src_b =
                block_for_label(&cfg, mphi->phi_incoming[mk].predecessor_label);
            if (src_b < 0)
              continue;
            int64_t tv =
                ct->has_cmp
                    ? (ct->phi_on_right
                           ? jt_eval_cmp(ct->cmp_op, ct->cmp_const, mpv)
                           : jt_eval_cmp(ct->cmp_op, mpv, ct->cmp_const))
                    : mpv;
            if ((ct->forced_polarity == 0 && tv != 0) ||
                (ct->forced_polarity == 1 && tv == 0))
              continue;
            size_t target = jt_pick_target(f, &cfg, through, term_i, tv);
            if (target != SIZE_MAX && target != through &&
                target != (size_t)src_b &&
                thread_edge_ex(f, &cfg, defs, (size_t)src_b, pred, mid, through,
                               target)) {
              if (jump_thread_trace_enabled())
                ny_dbg_heuristic_event(
                    "JUMP_THREAD",
                    "accept 2-hop edge b%d -> b%zu -> b%zu -> b%zu: constant "
                    "%lld",
                    src_b, mid, through, target, (long long)tv);
              threaded = true;
            }
          }
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
  size_t *label_pos =
      max_label >= 0 ? malloc(((size_t)max_label + 1) * sizeof(*label_pos))
                     : NULL;
  if (max_label >= 0 && (!last_jump || !label_pos)) {
    free(last_jump);
    free(label_pos);
    free(uses);
    free(defs);
    return false;
  }
  if (label_pos) {
    for (int64_t l = 0; l <= max_label; ++l)
      label_pos[l] = SIZE_MAX;
    for (size_t i = 0; i < f->len; ++i)
      if (f->data[i].op == NYIR_LABEL && f->data[i].imm >= 0 &&
          f->data[i].imm <= max_label)
        label_pos[f->data[i].imm] = i;
  }
  for (size_t i = 0; i < f->len; ++i)
    if ((f->data[i].op == NYIR_BR || f->data[i].op == NYIR_BR_IF) &&
        f->data[i].imm >= 0 && f->data[i].imm <= max_label)
      last_jump[f->data[i].imm] = i + 1;
  int64_t block_label = -1;
  size_t block_label_pos = 0;
  for (size_t i = 0; i < f->len; ++i) {
    nyr_inst_t *branch = &f->data[i];
    if (branch->op == NYIR_LABEL) {
      block_label = branch->imm;
      block_label_pos = i;
    }
    if (branch->op != NYIR_BR_IF || branch->a < 0 || (size_t)branch->a >= nv ||
        defs[branch->a] < 0 || uses[branch->a] != 1)
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
    if (block_label >= 0 && last_jump[block_label] > i + 1) {
      /*
       * Keep rotatable loop headers (where the fallthrough label is the loop
       * body). When the immediately following label is instead a loop exit
       * block (returns or jumps strictly outside the loop), inverting removes
       * the double-branch `je exit; jmp body` on every iteration.
       */
      bool fallthrough_exits = false;
      bool has_br_if = false;
      const nyr_inst_t *last_in = NULL;
      for (size_t k = label_at + 1;
           k < f->len && f->data[k].op != NYIR_LABEL; ++k) {
        if (f->data[k].op == NYIR_NOP)
          continue;
        if (f->data[k].op == NYIR_BR_IF) {
          has_br_if = true;
          break;
        }
        last_in = &f->data[k];
      }
      if (!has_br_if && last_in) {
        if (last_in->op == NYIR_RET) {
          fallthrough_exits = true;
        } else if (last_in->op == NYIR_BR && last_in->imm >= 0 &&
                   last_in->imm <= max_label && label_pos &&
                   label_pos[last_in->imm] != SIZE_MAX) {
          size_t tpos = label_pos[last_in->imm];
          if (tpos < block_label_pos || tpos >= last_jump[block_label])
            fallthrough_exits = true;
        }
      }
      if (!fallthrough_exits)
        continue;
    }
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
  free(label_pos);
  free(uses);
  free(defs);
  return true;
}
