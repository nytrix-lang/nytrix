/*
 * Conservative boolean PHI optimization.
 *
 * A two-arm, side-effect-free diamond whose condition is a proven constant
 * has only one reachable arm.  Replace its boolean join PHI with a COPY of
 * the selected constant value.  The CFG and the PHI predecessor labels are
 * left untouched; this keeps the transform verifier-safe even when CFG
 * cleanup runs later (or is disabled).
 */
#include "code/ir/opt/util.h"
#include "code/ir/internal.h"
#include <stdlib.h>

static size_t phiopt_label_pc(const nyir_func_t *f, int64_t label) {
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
static bool phiopt_empty_arm(const nyir_func_t *f, int64_t label,
                             int64_t join) {
  size_t pc = phiopt_label_pc(f, label);
  if (pc == SIZE_MAX)
    return false;
  bool saw_branch = false;
  for (++pc; pc < f->len; ++pc) {
    const nyir_inst_t *in = &f->data[pc];
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

static int64_t phiopt_join_label(const nyir_func_t *f, size_t phi_pc) {
  if (!f || phi_pc >= f->len)
    return -1;
  for (size_t i = phi_pc; i > 0; --i) {
    const nyir_inst_t *in = &f->data[i - 1];
    if (in->op == NYIR_LABEL)
      return in->imm;
    if (in->op != NYIR_NOP && in->op != NYIR_PHI)
      break;
  }
  return -1;
}
static bool phiopt_bool_const(const nyir_func_t *f, const int *defs,
                              const bool *known, const int64_t *values,
                              int value) {
  if (!f || !defs || !known || !values || value < 0 ||
      value >= f->next_value || !known[value])
    return false;
  for (unsigned depth = 0; depth < 16; ++depth) {
    int def_pc = defs[value];
    if (def_pc < 0 || (size_t)def_pc >= f->len)
      return false;
    const nyir_inst_t *def = &f->data[def_pc];
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
static bool phiopt_find_condition(const nyir_func_t *f, int64_t arm0,
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

bool nyir_phiopt(nyir_func_t *f) {
  if (!f || f->next_value <= 0)
    return true;
  bool *known = calloc((size_t)f->next_value, sizeof(*known));
  int64_t *values = calloc((size_t)f->next_value, sizeof(*values));
  int *defs = nyir_build_defs(f);
  if (!known || !values || !defs) {
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

  for (size_t i = 0; i < f->len; ++i) {
    nyir_inst_t *phi = &f->data[i];
    if (phi->op != NYIR_PHI || phi->dst < 0 ||
        phi->phi_incoming_len != 2 || !phi->phi_incoming)
      continue;
    nyir_phi_incoming_t p0 = phi->phi_incoming[0];
    nyir_phi_incoming_t p1 = phi->phi_incoming[1];
    if (!phiopt_bool_const(f, defs, known, values, p0.value) ||
        !phiopt_bool_const(f, defs, known, values, p1.value))
      continue;

    int cond = -1;
    int64_t true_arm = -1;
    int64_t join = phiopt_join_label(f, i);
    if (!phiopt_find_condition(f, p0.predecessor_label, p1.predecessor_label,
                               &cond, &true_arm) ||
        !phiopt_bool_const(f, defs, known, values, cond) ||
        !phiopt_empty_arm(f, p0.predecessor_label, join) ||
        !phiopt_empty_arm(f, p1.predecessor_label, join))
      continue;

    int64_t selected_label =
        values[cond]
            ? true_arm
            : (true_arm == p0.predecessor_label ? p1.predecessor_label
                                                 : p0.predecessor_label);
    int selected = selected_label == p0.predecessor_label
                       ? p0.value
                       : selected_label == p1.predecessor_label ? p1.value : -1;
    if (selected < 0)
      continue;
    free(phi->phi_incoming);
    phi->phi_incoming = NULL;
    phi->phi_incoming_len = 0;
    nir_make_copy(phi, selected);
  }

  free(known);
  free(values);
  free(defs);
  return true;
}
