/*
 * Loop unswitching: duplicates loop bodies when a loop-invariant
 * conditional branch can be evaluated once outside the loop.
 */
#include "code/native/ir/opt/util.h"
#include "code/native/ir/internal.h"
#include "base/compat.h"
#include "base/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Loop Unswitching: Move loop-invariant branches out of loops.
 *
 * Pattern:
 * for (...) {
 * if (invariant) { A } else { B }
 * ...
 * }
 *
 * Becomes:
 * if (invariant) {
 * for (...) { A; ... }
 * } else {
 * for (...) { B; ... }
 * }
 *
 * This enables LICM, vectorization, and other opts on specialized
 * loop bodies. Only unswitches BR_IF with invariant conditions.
 */

typedef struct {
  int head_label;
  size_t head_idx;
  size_t body_start;
  size_t back_edge;
  size_t end_idx;
} loop_t;

/*
 * Check if a value is defined inside a loop body.
 */
static bool value_defined_in_loop(const nyir_func_t *f, int value,
                                  const loop_t *lp) {
  for (size_t i = lp->body_start; i <= lp->back_edge; ++i) {
    if (f->data[i].dst == value)
      return true;
  }
  return false;
}

/*
 * Check if an instruction's operands are all loop-invariant.
 */
static __attribute__((unused)) bool operands_loop_invariant(const nyir_func_t *f, const nyir_inst_t *in,
                                    const loop_t *lp) {
  int inputs[] = {in->a, in->b, in->c, in->d, in->e, in->f};
  for (int k = 0; k < 6; ++k) {
    if (inputs[k] < 0)
      continue;
    if (value_defined_in_loop(f, inputs[k], lp))
      return false;
  }
  return true;
}

bool nyir_loop_unswitch(nyir_func_t *f) {
  if (!f || f->len < 8 || f->next_value <= 0)
    return true;

  nyir_cfg_t cfg = {0};
  if (!nyir_cfg_build(f, &cfg))
    return false;

  /*
   * Find all loops (same logic as LICM).
   */
  loop_t *loops = NULL;
  size_t loop_count = 0;
  size_t loop_cap = 0;

  for (size_t i = 0; i < f->len; ++i) {
    nyir_inst_t *in = &f->data[i];
    if (in->op != NYIR_BR || in->imm < 0)
      continue;
    int target = in->imm;
    for (size_t j = i; j > 0; --j) {
      if (f->data[j - 1].op == NYIR_LABEL && f->data[j - 1].imm == target) {
        size_t latch = cfg.inst_block[i];
        size_t header = cfg.inst_block[j - 1];
        if (!nyir_cfg_is_backedge(&cfg, latch, header))
          break;
        if (loop_count == loop_cap) {
          size_t cap = loop_cap ? loop_cap * 2 : 16;
          if (cap < loop_cap || cap > SIZE_MAX / sizeof(*loops)) {
            free(loops);
            nyir_cfg_free(&cfg);
            return false;
          }
          loop_t *grown = realloc(loops, cap * sizeof(*loops));
          if (!grown) {
            free(loops);
            nyir_cfg_free(&cfg);
            return false;
          }
          loops = grown;
          loop_cap = cap;
        }
        loops[loop_count].head_label = target;
        loops[loop_count].head_idx = j - 1;
        loops[loop_count].body_start = j;
        loops[loop_count].back_edge = i;
        loops[loop_count].end_idx = i + 1;
        for (size_t k = i + 1; k < f->len; ++k) {
          if (f->data[k].op == NYIR_LABEL) {
            loops[loop_count].end_idx = k;
            break;
          }
        }
        loop_count++;
        break;
      }
    }
  }

  if (loop_count == 0) {
    free(loops);
    nyir_cfg_free(&cfg);
    return true;
  }

  /*
   * Sort loops innermost first (by body_start descending).
   */
  for (size_t i = 0; i + 1 < loop_count; ++i) {
    for (size_t j = i + 1; j < loop_count; ++j) {
      if (loops[j].body_start > loops[i].body_start) {
        loop_t tmp = loops[i];
        loops[i] = loops[j];
        loops[j] = tmp;
      }
    }
  }


  for (size_t li = 0; li < loop_count; ++li) {
    loop_t *lp = &loops[li];

    /*
     * Find BR_IF with invariant condition in the loop body.
     */
    for (size_t i = lp->body_start; i <= lp->back_edge; ++i) {
      nyir_inst_t *br_if = &f->data[i];
      if (br_if->op != NYIR_BR_IF)
        continue;
      if (br_if->a < 0)
        continue;

      /*
       * Check if condition is loop-invariant.
       */
      bool all_outside = true;
      for (size_t j = lp->body_start; j <= lp->back_edge; ++j) {
        if (f->data[j].dst == br_if->a) {
          all_outside = false;
          break;
        }
      }
      if (!all_outside)
        continue;

      /*
       * Loop cloning is required to specialize an invariant branch.  Keeping
       * one edge here would silently change the program when the condition is
       * false, so leave the CFG unchanged until cloning is implemented.
       */
      continue;
    }
  }

  free(loops);
  nyir_cfg_free(&cfg);
  return true;
}