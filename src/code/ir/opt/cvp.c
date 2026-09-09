/*
 * Correlated Value Propagation (CVP): tracks equalities and affine
 * relationships between SSA values across basic blocks. Enables
 * replacing values with their correlated equivalents.
 *
 * References:
 *  Lerner, Grove, Chambers — "Composing Dataflow Analyses and
 *   Transformations" (POPL 2002). §3–4: CVP algorithm, lattice.
 *  Cooper & Torczon — Engineering a Compiler, 2nd Ed., Ch.10 §10.5:
 *   value numbering and correlated propagation.
 *  Muchnick — Advanced Compiler Design and Implementation, Ch.14 §14.2:
 *   value numbering and redundancy elimination.
 */
#include "code/ir/opt/util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  int eq_class;
  int64_t affine_a;
  int64_t affine_b;
  int base_var;
  bool has_affine;
} cvp_info_t;

static int cvp_find_rep(cvp_info_t *info, int v) {
  while (info[v].eq_class != v) {
    info[v].eq_class = info[info[v].eq_class].eq_class;
    v = info[v].eq_class;
  }
  return v;
}

static void cvp_union(cvp_info_t *info, int a, int b) {
  int ra = cvp_find_rep(info, a);
  int rb = cvp_find_rep(info, b);
  if (ra != rb) {
    info[rb].eq_class = ra;
  }
}

bool nyir_cvp(nyir_func_t *f) {
  if (!f || f->next_value <= 0)
    return true;

  int *defs = nyir_build_defs(f);
  if (!defs)
    return false;

  size_t n = (size_t)f->next_value;
  cvp_info_t *info = calloc(n, sizeof(*info));
  if (!info) {
    free(defs);
    return false;
  }

  for (size_t i = 0; i < n; ++i) {
    info[i].eq_class = (int)i;
    info[i].has_affine = false;
  }

  bool changed = false;

  for (size_t i = 0; i < f->len; ++i) {
    nyir_inst_t *in = &f->data[i];

    switch (in->op) {
    case NYIR_COPY:
      if (in->a >= 0 && in->dst >= 0) {
        cvp_union(info, in->dst, in->a);
        int ra = cvp_find_rep(info, in->a);
        if (info[ra].has_affine) {
          int rd = cvp_find_rep(info, in->dst);
          info[rd] = info[ra];
        }
      }
      break;

    case NYIR_CONST_I64:
      if (in->dst >= 0) {
        int rd = cvp_find_rep(info, in->dst);
        info[rd].has_affine = true;
        info[rd].affine_a = 0;
        info[rd].affine_b = in->imm;
        info[rd].base_var = -1;
      }
      break;

    case NYIR_ADD_I64:
      /*
       * Join facts only when BOTH sides carry proven facts. Every fact
       * this analysis produces is a pure constant (the base_var
       * machinery has no producer), and an unknown operand must never
       * stand in for constant zero -- its calloc-default affine_b is
       * 0, but the runtime value is arbitrary.
       */
      if (in->dst >= 0 && in->a >= 0 && in->b >= 0) {
        int ra = cvp_find_rep(info, in->a);
        int rb = cvp_find_rep(info, in->b);
        int rd = cvp_find_rep(info, in->dst);

        if (info[ra].has_affine && info[rb].has_affine &&
            info[ra].affine_a == 0 && info[rb].affine_a == 0) {
          info[rd].has_affine = true;
          info[rd].affine_a = 0;
          info[rd].affine_b = info[ra].affine_b + info[rb].affine_b;
          info[rd].base_var = -1;
        }
      }
      break;

    case NYIR_MUL_I64:
      if (in->dst >= 0 && in->a >= 0 && in->b >= 0) {
        int ra = cvp_find_rep(info, in->a);
        int rb = cvp_find_rep(info, in->b);
        int rd = cvp_find_rep(info, in->dst);

        if (info[ra].has_affine && info[rb].has_affine &&
            info[ra].affine_a == 0 && info[rb].affine_a == 0) {
          info[rd].has_affine = true;
          info[rd].affine_a = 0;
          info[rd].affine_b = info[ra].affine_b * info[rb].affine_b;
          info[rd].base_var = -1;
        }
      }
      break;

    case NYIR_SHL_I64:
      if (in->dst >= 0 && in->a >= 0 && in->b >= 0) {
        int ra = cvp_find_rep(info, in->a);
        int rb = cvp_find_rep(info, in->b);
        int rd = cvp_find_rep(info, in->dst);

        if (info[ra].has_affine && info[rb].has_affine &&
            info[ra].affine_a == 0 && info[rb].affine_a == 0 &&
            info[rb].affine_b >= 0 && info[rb].affine_b < 64) {
          info[rd].has_affine = true;
          info[rd].affine_a = 0;
          info[rd].affine_b =
              (__int128)info[ra].affine_b << info[rb].affine_b;
          info[rd].base_var = -1;
        }
      }
      break;

    case NYIR_CMP_I64:
      if (in->dst >= 0 && in->a >= 0 && in->b >= 0) {
        int ra = cvp_find_rep(info, in->a);
        int rb = cvp_find_rep(info, in->b);

        if (ra == rb) {
          int rd = cvp_find_rep(info, in->dst);
          if (in->cmp == NYIR_CMP_EQ) {
            info[rd].has_affine = true;
            info[rd].affine_a = 0;
            info[rd].affine_b = 1;
            info[rd].base_var = -1;
          } else if (in->cmp == NYIR_CMP_NE) {
            info[rd].has_affine = true;
            info[rd].affine_a = 0;
            info[rd].affine_b = 0;
            info[rd].base_var = -1;
          }
        }
      }
      break;

    case NYIR_PHI:
      /*
       * Join all incoming facts. This pass scans instructions once in
       * textual order, so a backedge input (or any input without a fact
       * yet) must poison the join instead of being skipped -- otherwise
       * a loop phi like φ(5, backedge) would be treated as the constant
       * init and every copy of it rewritten to 5.
       */
      if (in->dst >= 0 && in->phi_incoming) {
        int rd = cvp_find_rep(info, in->dst);
        bool have = false;
        bool joined = true;
        int64_t jb = 0;
        for (size_t k = 0; k < in->phi_incoming_len; ++k) {
          int val = in->phi_incoming[k].value;
          int rv = val >= 0 ? cvp_find_rep(info, val) : -1;
          if (rv < 0 || !info[rv].has_affine || info[rv].affine_a != 0) {
            joined = false;
            break;
          }
          if (!have) {
            have = true;
            jb = info[rv].affine_b;
          } else if (jb != info[rv].affine_b) {
            joined = false;
            break;
          }
        }
        info[rd].has_affine = joined && have;
        if (joined && have) {
          info[rd].affine_a = 0;
          info[rd].affine_b = jb;
          info[rd].base_var = -1;
        }
      }
      break;

    default:
      break;
    }
  }

  for (size_t i = 0; i < f->len; ++i) {
    nyir_inst_t *in = &f->data[i];
    int rd = in->dst >= 0 ? cvp_find_rep(info, in->dst) : -1;

    if (in->op == NYIR_LOAD_LOCAL || in->op == NYIR_COPY) {
      if (rd >= 0 && info[rd].has_affine && info[rd].affine_a == 0) {
        *in = (nyir_inst_t){.op = NYIR_CONST_I64,
                            .dst = in->dst,
                            .a = -1, .b = -1, .c = -1, .d = -1,
                            .imm = info[rd].affine_b,
                            .range = {.has_min = true, .has_max = true,
                                      .min = info[rd].affine_b,
                                      .max = info[rd].affine_b}};
        changed = true;
      }
    }
  }

  free(info);
  free(defs);
  return changed || true;
}