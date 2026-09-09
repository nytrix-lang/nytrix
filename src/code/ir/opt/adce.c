/*
 * ADCE: Aggressive Dead Code Elimination using control dependence.
 * Removes instructions whose results are not live, including those
 * in dead control flow not reachable from side-effecting operations.
 *
 * References:
 *  Cytron, Ferrante, Rosen, Wegman, Zadeck — "Efficiently Computing
 *   SSA Form and the Control Dependence Graph" (TOPLAS 1991). §5:
 *   control dependence and the CDG; §6: dead code via CDG.
 *  Wegman & Zadeck — "Constant Propagation with Conditional
 *   Branches" (TOPLAS 1991). SCCP unifies constant propagation
 *   + unreachable elimination. ADCE extends with post-dominator
 *   control-dependence pruning for side-effect-free ops.
 */
#include "code/ir/opt/util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Trace support.
 */
static bool nyir_adce_trace_enabled(void) {
  return ny_trace_enabled("NY_TRACE_ADCE");
}

static void nyir_adce_trace(const nyir_func_t *f, size_t at, const char *action,
                            const char *reason) {
  (void)f;
  if (!nyir_adce_trace_enabled())
    return;
  ny_trace_line("ADCE", "adce: %s @%zu: %s", action, at, reason);
}

/*
 * Check if an instruction has side effects that must be preserved.
 */
static bool inst_has_side_effects(const nyir_inst_t *in) {
  switch (in->op) {
  case NYIR_STORE_LOCAL:
  case NYIR_STORE_I64:
  case NYIR_CALL:
  case NYIR_BR:
  case NYIR_BR_IF:
  case NYIR_RET:
  case NYIR_ALLOCA:
  case NYIR_BOUNDS_CHECK:
  case NYIR_VEC4_STORE_F64:
  case NYIR_VEC8_STORE_F32:
  case NYIR_VEC4_STORE_I64:
  case NYIR_VEC8_STORE_I64:
  case NYIR_COPY_STRUCT:
    return true;
  default:
    /*
     * Stored effect annotations (VOLATILE/IO on loads etc.) are honored
     * by the caller via nyir_inst_effects().
     */
    return (nyir_inst_effects(in) &
            (NYIR_EFFECT_VOLATILE | NYIR_EFFECT_IO)) != 0;
  }
}

/*
 * Check if an instruction is a control flow instruction.
 */
static bool is_control_flow(const nyir_inst_t *in) {
  return in->op == NYIR_BR || in->op == NYIR_BR_IF || in->op == NYIR_RET;
}

/*
 * Mark all values that are live (used by side-effecting instructions or return).
 */
static void mark_live_values(const nyir_func_t *f, bool *live) {
  int *defs = nyir_build_defs(f);
  if (!defs)
    return;

  nyir_cfg_t cfg = {0};
  if (!nyir_cfg_build(f, &cfg)) {
    free(defs);
    return;
  }

  bool *block_reachable = calloc(cfg.block_count, sizeof(*block_reachable));
  if (!block_reachable) {
    free(defs);
    nyir_cfg_free(&cfg);
    return;
  }

  /*
   * Mark entry block as reachable.
   */
  size_t entry_block = cfg.inst_block[0];
  block_reachable[entry_block] = true;

  /*
   * Forward propagation of reachability.
   */
  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t b = 0; b < cfg.block_count; ++b) {
      if (!block_reachable[b])
        continue;
      for (size_t e = cfg.succ_offsets[b]; e < cfg.succ_offsets[b + 1]; ++e) {
        size_t succ = cfg.succ_blocks[e];
        if (!block_reachable[succ]) {
          block_reachable[succ] = true;
          changed = true;
        }
      }
    }
  }

  /*
   * Second pass: mark values used in live blocks.
   */
  for (size_t i = 0; i < f->len; ++i) {
    const nyir_inst_t *in = &f->data[i];
    /*
     * PHIs are never eliminated by this pass, so their incoming values must
     * be marked live even when the PHI sits in an unreachable block;
     * otherwise sweeping would leave dangling PHI operands behind.
     */
    if (in->op == NYIR_PHI && in->phi_incoming) {
      for (size_t k = 0; k < in->phi_incoming_len; ++k) {
        int v = in->phi_incoming[k].value;
        if (v >= 0 && v < f->next_value)
          live[v] = true;
      }
    }
    size_t block = cfg.inst_block[i];
    /*
     * Uses inside unreachable blocks do not keep pure definitions
     * alive, but every control-flow and side-effecting instruction is
     * retained unconditionally by the sweep below — so its operands
     * must be marked even when the block itself is unreachable, or the
     * sweep leaves dangling references behind.
     */
    if (!block_reachable[block] && !is_control_flow(in) &&
        !inst_has_side_effects(in))
      continue;

    if (is_control_flow(in) || inst_has_side_effects(in)) {
      /*
       * Calls keep up to six arguments in a..f; stopping at d leaves
       * args 5-6 dangling once the sweep NOPs their producers.
       */
      if (in->a >= 0 && in->a < f->next_value)
        live[in->a] = true;
      if (in->b >= 0 && in->b < f->next_value)
        live[in->b] = true;
      if (in->c >= 0 && in->c < f->next_value)
        live[in->c] = true;
      if (in->d >= 0 && in->d < f->next_value)
        live[in->d] = true;
      if (in->e >= 0 && in->e < f->next_value)
        live[in->e] = true;
      if (in->f >= 0 && in->f < f->next_value)
        live[in->f] = true;
      /*
       * Arguments past the six register slots ride in extra_args; their
       * producers must survive the sweep as well.
       */
      if (in->op == NYIR_CALL && in->extra_args) {
        int want = in->imm > 6 ? in->imm - 6 : 0;
        for (int k = 0; k < want; ++k)
          if (in->extra_args[k] >= 0 && in->extra_args[k] < f->next_value)
            live[in->extra_args[k]] = true;
      }
    }
  }

  /*
   * Third pass: propagate liveness backwards through definitions.
   */
  changed = true;
  while (changed) {
    changed = false;
    for (size_t i = 0; i < f->len; ++i) {
      const nyir_inst_t *in = &f->data[i];
      if (in->dst < 0 || in->dst >= f->next_value)
        continue;
      if (live[in->dst]) {
        if (in->a >= 0 && in->a < f->next_value && !live[in->a]) {
          live[in->a] = true;
          changed = true;
        }
        if (in->b >= 0 && in->b < f->next_value && !live[in->b]) {
          live[in->b] = true;
          changed = true;
        }
        if (in->c >= 0 && in->c < f->next_value && !live[in->c]) {
          live[in->c] = true;
          changed = true;
        }
        if (in->d >= 0 && in->d < f->next_value && !live[in->d]) {
          live[in->d] = true;
          changed = true;
        }
        if (in->e >= 0 && in->e < f->next_value && !live[in->e]) {
          live[in->e] = true;
          changed = true;
        }
        if (in->f >= 0 && in->f < f->next_value && !live[in->f]) {
          live[in->f] = true;
          changed = true;
        }
        if (in->op == NYIR_CALL && in->extra_args) {
          int want = in->imm > 6 ? in->imm - 6 : 0;
          for (int k = 0; k < want; ++k) {
            int v = in->extra_args[k];
            if (v >= 0 && v < f->next_value && !live[v]) {
              live[v] = true;
              changed = true;
            }
          }
        }
        if (in->op == NYIR_PHI && in->phi_incoming) {
          for (size_t k = 0; k < in->phi_incoming_len; ++k) {
            int v = in->phi_incoming[k].value;
            if (v >= 0 && v < f->next_value && !live[v]) {
              live[v] = true;
              changed = true;
            }
          }
        }
      }
    }
  }

  free(block_reachable);
  free(defs);
  nyir_cfg_free(&cfg);
}

/*
 * Main ADCE pass.
 * Removes instructions whose results are not live.
 */
bool nyir_adce(nyir_func_t *f) {
  if (!f || f->next_value <= 0)
    return true;

  bool *live = calloc((size_t)f->next_value, sizeof(*live));

  if (!live)
    return false;

  mark_live_values(f, live);

  for (size_t i = 0; i < f->len; ++i) {
    nyir_inst_t *in = &f->data[i];
    if (in->dst < 0 || in->dst >= f->next_value)
      continue;

    if (inst_has_side_effects(in) || is_control_flow(in))
      continue;

    if (in->op == NYIR_PHI)
      continue;

    if (!live[in->dst]) {
      nyir_adce_trace(f, i, "eliminate", "result not live");
      nyir_inst_discard(in);
    }
  }

  free(live);
  return true;
}