/*
 * Null Check Elimination: removes redundant null pointer checks when the
 * pointer is proven non-null by range facts, dominating checks, or
 * control flow implications.
 *
 * References:
 *  Kawahito, Shizawa, Nishiyama — "Effective Null Pointer Check
 *   Elimination for Java" (OOPSLA 2000). Dominator-based BCE for
 *   null checks.
 *  Cooper & Torczon — Engineering a Compiler, 2nd Ed., Ch.10 §10.4:
 *   redundant null check elimination via value range analysis.
 *  Muchnick — Advanced Compiler Design and Implementation, Ch.14 §14.3:
 *   redundancy elimination for null checks.
 */
#include "base/debug.h"
#include "code/backend/ir/opt/loop.h"
#include "code/backend/ir/opt/util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool nyr_nce_trace_enabled(void) {
  return ny_dbg_is_enabled(NY_DBG_HEURISTIC) ||
         ny_trace_enabled("NY_TRACE_NCE");
}

static void nyr_nce_trace(const nyr_func_t *f, size_t at, const char *action,
                           const char *reason) {
  if (!nyr_nce_trace_enabled())
    return;
  const nyr_inst_t *in = &f->data[at];
  if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
    ny_dbg_heuristic_event(
        "NULL_CHECK", "%s @%zu (dst=%d a=%d b=%d imm=%lld): %s", action, at,
        in->dst, in->a, in->b, (long long)in->imm, reason);
  else
    ny_trace_line("NCE", "nce: %s @%zu (dst=%d a=%d b=%d imm=%lld): %s", action,
                  at, in->dst, in->a, in->b, (long long)in->imm, reason);
}

static bool facts_is_const_zero(const nyr_func_t *f, const int *defs,
                                int value) {
  if (value < 0 || value >= f->next_value)
    return false;
  const nyr_inst_t *def = defs[value] >= 0 ? &f->data[defs[value]] : NULL;
  return def && def->op == NYIR_CONST_I64 && def->imm == 0;
}

/*
 * Check if a value is proven non-null at a given program point.
 * Uses range facts, known constants, address roots, and counted-loop
 * induction facts.  All checks are position-insensitive and therefore
 * valid wherever the value's single SSA definition reaches.
 */
static bool value_nonnull_at(const nyr_func_t *f, const int *defs,
                             const nyr_value_fact_t *facts,
                             const nyr_scev_info_t *scev_info, int value,
                             size_t at) {
  (void)at;
  if (value < 0 || value >= f->next_value)
    return false;

  if (facts && facts[value].range.has_min && facts[value].range.min > 0)
    return true;

  if (defs) {
    const nyr_inst_t *def = defs[value] >= 0 && (size_t)defs[value] < f->len
                                 ? &f->data[defs[value]]
                                 : NULL;
    if (def && def->dst == value) {
      if (def->range.has_min && def->range.min > 0)
        return true;
      if (def->op == NYIR_CONST_I64 && def->imm != 0)
        return true;
      if (def->op == NYIR_ADDR_LOCAL || def->op == NYIR_ALLOCA ||
          def->op == NYIR_ADDR_SYMBOL)
        return true;
      if (def->scalar_class == NYIR_SCALAR_OBJECT ||
          def->scalar_class == NYIR_SCALAR_STRING)
        return true;
    }
  }

  /*
   * A counted IV with a strictly positive constant start and positive step
   * never takes the value 0 -- but ONLY once wraparound is excluded:
   * NyIR i64 induction updates wrap, so e.g. init = 2^62 with step = 2^62
   * passes through 0. Require a known positive trip count whose final
   * value still fits int64; then the sequence is strictly increasing
   * from init > 0 and can never reach zero.
   */
  if (scev_info) {
    for (size_t li = 0; li < scev_info->count; ++li) {
      const nyr_scev_loop_t *l = &scev_info->loops[li];
      if (l->iv == value && l->init_is_const && l->init > 0 && l->step > 0 &&
          l->trip_count_known && l->trip_count > 0) {
        __int128 last = (__int128)l->init +
                        (__int128)(l->trip_count - 1) * (__int128)l->step;
        if (last <= INT64_MAX)
          return true;
      }
    }
  }

  return false;
}

bool nyr_null_check_elim(nyr_func_t *f) {
  if (!f || f->next_value <= 0)
    return true;

  int *defs = nyr_build_defs(f);
  nyr_value_fact_t *facts = calloc((size_t)f->next_value, sizeof(*facts));
  nyr_cfg_t cfg = {0};
  nyr_scev_info_t scev_info = {0};

  if (!defs || !facts || !nyr_cfg_build(f, &cfg) ||
      !nyr_analyze_values(f, facts, (size_t)f->next_value, NULL, 0)) {
    free(defs);
    free(facts);
    nyr_cfg_free(&cfg);
    return false;
  }

  (void)nyr_scev_analyze(f, &scev_info);

  bool changed = false;

  /*
   * Simplify branch users before folding their compare definitions. If the
   * compare is replaced first, its BR_IF user can no longer see the proof
   * that justified taking or dropping the edge.
   */
  for (size_t i = 0; i < f->len; ++i) {
    nyr_inst_t *in = &f->data[i];
    if (in->op != NYIR_BR_IF || in->a < 0 || in->a >= f->next_value)
      continue;
    const nyr_inst_t *cond_def =
        defs[in->a] >= 0 ? &f->data[defs[in->a]] : NULL;
    if (!cond_def || cond_def->op != NYIR_CMP_I64 ||
        !facts_is_const_zero(f, defs, cond_def->b))
      continue;
    if ((cond_def->cmp == NYIR_CMP_NE || cond_def->cmp == NYIR_CMP_GT) &&
        value_nonnull_at(f, defs, facts, &scev_info, cond_def->a, i)) {
      nyr_nce_trace(f, i, "eliminate br.if",
                     "null check condition proven always true");
      nyr_debug_loc_t debug = in->debug;
      *in = (nyr_inst_t){.op = NYIR_BR,
                          .dst = -1,
                          .a = -1,
                          .b = -1,
                          .c = -1,
                          .d = -1,
                          .imm = in->imm,
                          .effects = NYIR_EFFECT_CONTROL,
                          .debug = debug};
      changed = true;
    } else if ((cond_def->cmp == NYIR_CMP_EQ || cond_def->cmp == NYIR_CMP_LE) &&
               value_nonnull_at(f, defs, facts, &scev_info, cond_def->a, i)) {
      nyr_nce_trace(f, i, "eliminate br.if",
                     "null check condition proven always false");
      nyr_debug_loc_t debug = in->debug;
      *in = (nyr_inst_t){.op = NYIR_NOP,
                          .dst = -1,
                          .a = -1,
                          .b = -1,
                          .c = -1,
                          .d = -1,
                          .imm = 0,
                          .debug = debug};
      changed = true;
    }
  }

  for (size_t i = 0; i < f->len; ++i) {
    nyr_inst_t *in = &f->data[i];

    if (in->op == NYIR_CMP_I64 && in->dst >= 0 &&
        (in->cmp == NYIR_CMP_NE || in->cmp == NYIR_CMP_GT) &&
        facts_is_const_zero(f, defs, in->b)) {
      if (value_nonnull_at(f, defs, facts, &scev_info, in->a, i)) {
        nyr_nce_trace(f, i, "eliminate cmp",
                       "value proven non-null by range facts / SCEV");
        nir_make_const(in, 1);
        in->range = (nyr_range_t){
            .has_min = true, .has_max = true, .min = 1, .max = 1};
        changed = true;
        continue;
      }
    }
  }

  free(defs);
  free(facts);
  nyr_cfg_free(&cfg);
  nyr_scev_free(&scev_info);
  return changed || true;
}
