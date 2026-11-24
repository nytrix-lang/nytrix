/*
 * Conservative boolean PHI optimization.
 *
 * A two-arm, side-effect-free diamond whose condition is a proven constant
 * has only one reachable arm.  Replace its boolean join PHI with a COPY of
 * the selected constant value.  The CFG and the PHI predecessor labels are
 * left untouched; this keeps the transform verifier-safe even when CFG
 * cleanup runs later (or is disabled).
 */
#include "base/debug.h"
#include "code/backend/core/nyr.h"
#include "code/backend/passes/util.h"
#include <stdlib.h>

static size_t phiopt_label_pc(const nyr_func_t *f, int64_t label) {
  if (!f)
    return SIZE_MAX;
  for (size_t i = 0; i < f->len; ++i)
    if (f->data[i].op == NYIR_LABEL && f->data[i].imm == label)
      return i;
  return SIZE_MAX;
}

/*
 * An arm is deliberately restricted to LABEL, NOPs, and BR join.
 */
static bool phiopt_empty_arm(const nyr_func_t *f, int64_t label, int64_t join) {
  size_t pc = phiopt_label_pc(f, label);
  if (pc == SIZE_MAX)
    return false;
  bool saw_branch = false;
  for (++pc; pc < f->len; ++pc) {
    const nyr_inst_t *in = &f->data[pc];
    if (in->op == NYIR_NOP)
      continue;
    if (in->op == NYIR_LABEL)
      break;
    if (in->op != NYIR_BR || saw_branch || in->imm != join)
      return false;
    saw_branch = true;
  }
  return saw_branch;
}

static int64_t phiopt_join_label(const nyr_func_t *f, size_t phi_pc) {
  if (!f || phi_pc >= f->len)
    return -1;
  for (size_t i = phi_pc; i > 0; --i) {
    const nyr_inst_t *in = &f->data[i - 1];
    if (in->op == NYIR_LABEL)
      return in->imm;
    if (in->op != NYIR_NOP && in->op != NYIR_PHI)
      break;
  }
  return -1;
}
static bool phiopt_bool_const(const nyr_func_t *f, const int *defs,
                              const bool *known, const int64_t *values,
                              int value) {
  if (!f || !defs || !known || !values || value < 0 || value >= f->next_value ||
      !known[value])
    return false;
  for (unsigned depth = 0; depth < 16; ++depth) {
    int def_pc = defs[value];
    if (def_pc < 0 || (size_t)def_pc >= f->len)
      return false;
    const nyr_inst_t *def = &f->data[def_pc];
    if (def->op == NYIR_CONST_I64)
      return values[value] == 0 || values[value] == 1;
    if (def->op != NYIR_COPY || def->a < 0 || def->a >= f->next_value ||
        !known[def->a])
      return false;
    value = def->a;
  }
  return false;
}

/*
 * Find a terminator BR_IF whose true and false successors are the two arms.
 */
static bool phiopt_find_condition(const nyr_func_t *f, int64_t arm0,
                                  int64_t arm1, int *cond_out,
                                  int64_t *true_arm_out) {
  if (!f || !cond_out || !true_arm_out)
    return false;
  for (size_t i = 0; i < f->len; ++i) {
    if (f->data[i].op != NYIR_BR_IF)
      continue;
    size_t next = i + 1;
    while (next < f->len && f->data[next].op == NYIR_NOP)
      ++next;
    if (next >= f->len || f->data[next].op != NYIR_LABEL)
      continue;
    int64_t fallthrough = f->data[next].imm;
    if (f->data[i].imm != arm0 && f->data[i].imm != arm1)
      continue;
    if ((fallthrough != arm0 && fallthrough != arm1) ||
        fallthrough == f->data[i].imm)
      continue;
    *cond_out = f->data[i].a;
    *true_arm_out = f->data[i].imm;
    return true;
  }
  return false;
}

static int phiopt_resolve_copy(const nyr_func_t *f, const int *defs, int v) {
  for (unsigned steps = 0; steps < 8; ++steps) {
    if (v < 0 || v >= f->next_value || defs[v] < 0)
      return v;
    const nyr_inst_t *in = &f->data[(size_t)defs[v]];
    if (in->op != NYIR_COPY)
      return v;
    v = in->a;
  }
  return v;
}

bool nyr_phiopt(nyr_func_t *f) {
  if (!f || f->next_value <= 0)
    return true;
  bool trace = ny_dbg_is_enabled(NY_DBG_HEURISTIC);
  size_t candidates = 0, malformed = 0, nonconstant_values = 0;
  size_t missing_condition = 0, nonconstant_condition = 0;
  size_t nonempty_arms = 0, invalid_selection = 0, rewritten = 0;
  bool *known = calloc((size_t)f->next_value, sizeof(*known));
  int64_t *values = calloc((size_t)f->next_value, sizeof(*values));
  int *defs = nyr_build_defs(f);
  nyr_cfg_t cfg = {0};
  if (!known || !values || !defs || !nyr_cfg_build(f, &cfg)) {
    free(known);
    free(values);
    free(defs);
    nyr_cfg_free(&cfg);
    return false;
  }
  bool *made = calloc(f->len ? f->len : 1, sizeof(*made));
  bool any_made = false;
  bool any_trivial = false;
  for (bool fold_again = true; fold_again;) {
    fold_again = false;
    for (size_t i = 0; i < f->len; ++i) {
      nyr_inst_t *phi = &f->data[i];
      if (phi->op != NYIR_PHI || phi->dst < 0 || !phi->phi_incoming ||
          phi->phi_incoming_len == 0)
        continue;
      int unique_v = -1;
      bool uniform = true;
      for (size_t k = 0; k < phi->phi_incoming_len; ++k) {
        int v = phiopt_resolve_copy(f, defs, phi->phi_incoming[k].value);
        if (v < 0 || v == phi->dst)
          continue;
        if (unique_v < 0)
          unique_v = v;
        else if (v != unique_v) {
          uniform = false;
          break;
        }
      }
      if (!uniform || unique_v < 0 || unique_v >= f->next_value ||
          defs[unique_v] < 0)
        continue;
      size_t def_idx = (size_t)defs[unique_v];
      size_t def_block = cfg.inst_block[def_idx];
      size_t phi_block = cfg.inst_block[i];
      bool dominates = def_block == phi_block
                           ? def_idx < i
                           : nyr_cfg_dominates(&cfg, def_block, phi_block);
      if (!dominates)
        continue;
      nir_make_copy(phi, unique_v);
      if (made)
        made[i] = true;
      any_made = true;
      fold_again = true;
      any_trivial = true;
      if (trace)
        rewritten++;
    }
  }
  if (any_trivial) {
    free(defs);
    defs = nyr_build_defs(f);
    if (!defs) {
      free(known);
      free(values);
      free(made);
      nyr_cfg_free(&cfg);
      return false;
    }
  }
  nyr_cfg_free(&cfg);
  if (!nir_collect_consts(f, known, values)) {
    free(known);
    free(values);
    free(defs);
    free(made);
    return false;
  }

  for (size_t i = 0; i < f->len; ++i) {
    nyr_inst_t *phi = &f->data[i];
    if (phi->op != NYIR_PHI)
      continue;
    if (trace)
      candidates++;
    if (phi->dst < 0 || phi->phi_incoming_len != 2 || !phi->phi_incoming) {
      if (trace)
        malformed++;
      continue;
    }
    nyr_phi_incoming_t p0 = phi->phi_incoming[0];
    nyr_phi_incoming_t p1 = phi->phi_incoming[1];
    if (!phiopt_bool_const(f, defs, known, values, p0.value) ||
        !phiopt_bool_const(f, defs, known, values, p1.value)) {
      if (trace)
        nonconstant_values++;
      continue;
    }

    int cond = -1;
    int64_t true_arm = -1;
    int64_t join = phiopt_join_label(f, i);
    if (!phiopt_find_condition(f, p0.predecessor_label, p1.predecessor_label,
                               &cond, &true_arm)) {
      if (trace)
        missing_condition++;
      continue;
    }
    if (!phiopt_bool_const(f, defs, known, values, cond)) {
      if (trace)
        nonconstant_condition++;
      continue;
    }
    if (!phiopt_empty_arm(f, p0.predecessor_label, join) ||
        !phiopt_empty_arm(f, p1.predecessor_label, join)) {
      if (trace)
        nonempty_arms++;
      continue;
    }

    int64_t selected_label = values[cond] ? true_arm
                                          : (true_arm == p0.predecessor_label
                                                 ? p1.predecessor_label
                                                 : p0.predecessor_label);
    int selected = selected_label == p0.predecessor_label   ? p0.value
                   : selected_label == p1.predecessor_label ? p1.value
                                                            : -1;
    if (selected < 0) {
      if (trace)
        invalid_selection++;
      continue;
    }
    nir_make_copy(phi, selected);
    if (made)
      made[i] = true;
    any_made = true;
    if (trace)
      rewritten++;
  }

  /*
   * A COPY that replaced the first of several PHIs would precede the rest
   * (verifier: "phi is not at the start of its block"). Its operand is a
   * constant defined elsewhere, so sink it below the block's PHI run.
   */
  for (size_t i = 0; any_made && made && i < f->len; ++i) {
    if (!made[i])
      continue;
    size_t end = i + 1;
    while (end < f->len &&
           (f->data[end].op == NYIR_PHI || f->data[end].op == NYIR_NOP ||
            made[end]))
      ++end;
    size_t w = i;
    nyr_inst_t *tmp = malloc((end - i) * sizeof(*tmp));
    bool *tmp_made = malloc((end - i) * sizeof(*tmp_made));
    if (!tmp || !tmp_made) {
      free(tmp);
      free(tmp_made);
      break;
    }
    size_t t = 0;
    for (size_t k = i; k < end; ++k)
      if (f->data[k].op == NYIR_PHI)
        f->data[w++] = f->data[k];
      else {
        tmp[t] = f->data[k];
        tmp_made[t++] = made[k];
      }
    for (size_t k = 0; k < t; ++k) {
      made[w] = false;
      f->data[w++] = tmp[k];
    }
    for (size_t k = i; k < end; ++k)
      made[k] = false;
    free(tmp);
    free(tmp_made);
    i = end - 1;
  }
  free(made);

  if (trace)
    ny_dbg_heuristic_event(
        "PHIOPT",
        "candidates=%zu rewritten=%zu malformed=%zu nonconstant-values=%zu "
        "missing-branch-condition=%zu nonconstant-condition=%zu "
        "nonempty-arms=%zu invalid-selection=%zu",
        candidates, rewritten, malformed, nonconstant_values, missing_condition,
        nonconstant_condition, nonempty_arms, invalid_selection);

  free(known);
  free(values);
  free(defs);
  return true;
}
