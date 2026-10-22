/*
 * Advanced NYIR lowering: transforms high-level NYIR ops into
 * architecture-neutral optimized forms before machine-form conversion.
 *
 * This module owns bounds-check elimination (BCE), loop unrolling,
 * strength reduction, inlining heuristics, block layout, and
 * profile-guided code layout.  It is the main compilation driver
 * after the pure optimizer.
 *
 * References:
 *  Cooper & Torczon — Engineering a Compiler, 2nd Ed., Ch.8-9
 *   (optimization overview, inlining, pass structure).
 *  Fog — Optimizing subroutines in C++ (Vol.1), Ch.12
 *   (loop unrolling, IV classification, vectorization heuristics).
 *  Muchnick — Advanced Compiler Design and Implementation, Ch.14
 *   (loop transforms, loop nests, strength reduction).
 *  Warren — Hacker's Delight, 2nd Ed., Ch.10
 *   (division by constants, Granlund-Montgomery algorithm).
 *  Granlund & Montgomery — "Division by Invariant Integers using
 *   Multiplication" (PLDI 1994).
 *  Pop, Cohen, Silber — "Induction Variable Analysis with Delayed
 *   Abstractions" (INRIA TR, 2004).  SCEV classification.
 */
#include "base/common.h"
#include "base/compat.h"
#include "base/debug.h"
#include "code/backend/ir/internal.h"
#include "code/backend/ir/opt/loop.h"
#include "code/backend/ir/opt/util.h"
#include <inttypes.h>
#include <limits.h>
#include <nyr/ir.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Advanced optimizer seeds for the tiered-compiler roadmap.
 * Each pass is intentionally bounded, verifier-friendly, and gated by
 * opt level. Full research-scale versions stay future work.
 */

/*
 * Null/alignment fact seed: after `x != 0` / `x > 0` compare+branch, mark the
 * compared value's range as excluding zero when the true edge is taken. We
 * only strengthen instruction-local range metadata on the compare result and
 * on proven non-zero AND masks (low-bit alignment).
 */
bool nyr_null_align_facts(nyr_func_t *f) {
  if (!f || f->next_value <= 0)
    return true;
  nyr_value_fact_t *facts =
      ny_calloc_array((size_t)f->next_value, sizeof(*facts));
  if (!facts ||
      !nyr_analyze_values(f, facts, (size_t)f->next_value, NULL, 0)) {
    free(facts);
    return false;
  }
  for (size_t i = 0; i < f->len; ++i) {
    nyr_inst_t *in = &f->data[i];
    /*
     * (x & (align-1)) == 0 for power-of-two align → alignment fact on range of
     * the AND result is already 0..mask; nothing to rewrite. Strengthen known
     * non-null: if a value is proven > 0 by range, attach min=1 when missing.
     */
    if (in->dst >= 0 && in->dst < f->next_value) {
      const nyr_range_t *r = &facts[in->dst].range;
      if (r->has_min && r->min > 0 && !in->range.has_min) {
        in->range.has_min = true;
        in->range.min = r->min;
        if (r->has_max) {
          in->range.has_max = true;
          in->range.max = r->max;
        }
      }
    }
    /*
     * Known-bits upper bound: x & non-negative mask is always in
     * [0, mask], even when the mask is not the all-low-bits form.
     * This is the useful fact for parity/bit-field consumers.
     */
    if (in->op == NYIR_AND_I64 && in->b >= 0 && in->b < f->next_value &&
        facts[in->b].known_const && facts[in->b].const_value >= 0) {
      int64_t mask = facts[in->b].const_value;
      in->range.has_min = true;
      in->range.has_max = true;
      in->range.min = 0;
      in->range.max = mask;
    }
    int64_t rhs_singleton = 0;
    bool rhs_is_singleton =
        in->b >= 0 && in->b < f->next_value &&
        nyr_value_fact_singleton(&facts[in->b], &rhs_singleton);
    if (in->op == NYIR_MOD_I64 && rhs_is_singleton && rhs_singleton != 0 &&
        rhs_singleton != INT64_MIN && facts[in->a].range.has_min &&
        facts[in->a].range.min >= 0) {
      int64_t mod = rhs_singleton < 0 ? -rhs_singleton : rhs_singleton;
      in->range.has_min = true;
      in->range.has_max = true;
      in->range.min = 0;
      in->range.max = mod - 1;
      /*
       * A non-negative dividend that cannot reach the modulus is unchanged
       * by the operation (a % m == a), so keep the tighter dividend bound
       * instead of widening to m-1.
       */
      if (facts[in->a].range.has_max && facts[in->a].range.max < mod - 1)
        in->range.max = facts[in->a].range.max;
    }
    if (in->op == NYIR_SAR_I64 && rhs_is_singleton && rhs_singleton >= 0 &&
        rhs_singleton < 64 && facts[in->a].range.has_min &&
        facts[in->a].range.min >= 0 && facts[in->a].range.has_max) {
      unsigned shift = (unsigned)rhs_singleton;
      in->range.has_min = true;
      in->range.has_max = true;
      in->range.min = facts[in->a].range.min >> shift;
      in->range.max = facts[in->a].range.max >> shift;
    }
    if (in->op == NYIR_SHL_I64 && rhs_is_singleton && rhs_singleton >= 0 &&
        rhs_singleton < 63 && facts[in->a].range.has_min &&
        facts[in->a].range.min >= 0 && facts[in->a].range.has_max &&
        facts[in->a].range.max <= (INT64_MAX >> (unsigned)rhs_singleton)) {
      unsigned shift = (unsigned)rhs_singleton;
      in->range.has_min = true;
      in->range.has_max = true;
      in->range.min = facts[in->a].range.min << shift;
      in->range.max = facts[in->a].range.max << shift;
    }
  }
  free(facts);
  return true;
}

/*
 * Induction simplification seed: i = i + C with C const → keep as add (already
 * strength-reduced when C is power-of-two related). Detect `x - x` style and
 * `phi` self-add of constant step already handled by SCCP/peephole. Here we
 * fold `(i + c) - c` and `(i - c) + c` via local e-graph style peep.
 */
bool nyr_iv_simplify(nyr_func_t *f) {
  if (!f || f->len < 2)
    return true;
  for (size_t i = 0; i < f->len; ++i) {
    nyr_inst_t *in = &f->data[i];
    if (in->dst < 0 || in->a < 0 || in->b < 0)
      continue;
    /*
     * (x + c) - c  or  (x - c) + c → x when c is the same SSA const use.
     */
    if (in->op != NYIR_ADD_I64 && in->op != NYIR_SUB_I64)
      continue;
    /*
     * Look for def of a as ADD/SUB with same const b.
     */
    for (size_t j = i; j > 0; --j) {
      const nyr_inst_t *def = &f->data[j - 1];
      if (def->dst != in->a)
        continue;
      if (def->op == NYIR_ADD_I64 && in->op == NYIR_SUB_I64 &&
          def->b == in->b && def->b >= 0) {
        nir_make_copy(in, def->a);
      } else if (def->op == NYIR_SUB_I64 && in->op == NYIR_ADD_I64 &&
                 def->b == in->b && def->b >= 0) {
        nir_make_copy(in, def->a);
      }
      break;
    }
  }
  return true;
}

static int nyr_value_skip_copies(const nyr_func_t *f, const int *defs,
                                  int value) {
  for (int depth = 0;
       f && defs && value >= 0 && value < f->next_value && depth < 16;
       ++depth) {
    int def_i = defs[value];
    if (def_i < 0)
      break;
    const nyr_inst_t *def = &f->data[def_i];
    if (def->op != NYIR_COPY || def->a < 0)
      break;
    value = def->a;
  }
  return value;
}

static bool nyr_const_i64_value(const nyr_func_t *f, const int *defs,
                                 int value, int64_t *out) {
  value = nyr_value_skip_copies(f, defs, value);
  if (!f || !defs || value < 0 || value >= f->next_value)
    return false;
  int def_i = defs[value];
  if (def_i < 0)
    return false;
  const nyr_inst_t *def = &f->data[def_i];
  if (def->op != NYIR_CONST_I64)
    return false;
  if (out)
    *out = def->imm;
  return true;
}

static bool nyr_scaled_base(const nyr_func_t *f, const int *defs, int value,
                             int *base_out, unsigned *scale_out) {
  value = nyr_value_skip_copies(f, defs, value);
  if (!f || !defs || value < 0 || value >= f->next_value)
    return false;
  int def_i = defs[value];
  if (def_i >= 0) {
    const nyr_inst_t *def = &f->data[def_i];
    int64_t scale = 0;
    if (def->op == NYIR_SHL_I64 &&
        nyr_const_i64_value(f, defs, def->b, &scale) && scale >= 0 &&
        scale < 31) {
      *base_out = nyr_value_skip_copies(f, defs, def->a);
      *scale_out = (unsigned)1u << (unsigned)scale;
      return true;
    }
    if (def->op == NYIR_MUL_I64) {
      if (nyr_const_i64_value(f, defs, def->a, &scale) && scale > 0 &&
          (uint64_t)scale <= UINT_MAX) {
        *base_out = nyr_value_skip_copies(f, defs, def->b);
        *scale_out = (unsigned)scale;
        return true;
      }
      if (nyr_const_i64_value(f, defs, def->b, &scale) && scale > 0 &&
          (uint64_t)scale <= UINT_MAX) {
        *base_out = nyr_value_skip_copies(f, defs, def->a);
        *scale_out = (unsigned)scale;
        return true;
      }
    }
  }
  *base_out = value;
  *scale_out = 1;
  return true;
}

static bool nyr_values_same_at(const nyr_func_t *f, const int *defs, int a,
                                int b, size_t at) {
  a = nyr_value_skip_copies(f, defs, a);
  b = nyr_value_skip_copies(f, defs, b);
  return (a >= 0 && a == b) || nir_operands_same_value(f, a, b, at);
}

/*
 * Loop guards and lowered container checks often reload the same local in
 * adjacent CFG blocks.  Generic operand equivalence is intentionally more
 * conservative and stops at branch/label boundaries, but for BCE we may
 * cross those boundaries when the local itself cannot change.  We also
 * recognize immutable alias locals (for example a list-length fact slot and
 * the source bound slot) when both are assigned exactly once from the same
 * value.  Address-taken locals are excluded so calls cannot invalidate the
 * proof behind our back.
 */
static bool nyr_guard_local_address_taken(const nyr_func_t *f, int64_t slot) {
  if (!f || slot < 0)
    return true;
  for (size_t i = 0; i < f->len; ++i)
    if (f->data[i].op == NYIR_ADDR_LOCAL && f->data[i].imm == slot)
      return true;
  return false;
}

static bool nyr_guard_unique_local_store(const nyr_func_t *f, int64_t slot,
                                          int *value_out) {
  if (!f || slot < 0 || !value_out || nyr_guard_local_address_taken(f, slot))
    return false;
  bool found = false;
  int value = -1;
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->op != NYIR_STORE_LOCAL || in->imm != slot)
      continue;
    if (found)
      return false;
    found = true;
    value = in->a;
  }
  if (!found)
    return false;
  *value_out = value;
  return true;
}

static bool nyr_guard_loads_same_local_value(const nyr_func_t *f,
                                              const int *defs, int a, int b,
                                              size_t at) {
  if (!f || !defs || a < 0 || b < 0 || a >= f->next_value || b >= f->next_value)
    return false;
  int ai = defs[a], bi = defs[b];
  if (ai < 0 || bi < 0 || (size_t)ai >= f->len || (size_t)bi >= f->len)
    return false;
  const nyr_inst_t *da = &f->data[ai];
  const nyr_inst_t *db = &f->data[bi];
  if (da->op != NYIR_LOAD_LOCAL || db->op != NYIR_LOAD_LOCAL)
    return false;

  if (da->imm == db->imm) {
    if (nyr_guard_local_address_taken(f, da->imm))
      return false;
    size_t lo = (size_t)(ai < bi ? ai : bi);
    size_t hi = (size_t)(ai < bi ? bi : ai);
    for (size_t i = lo + 1; i < hi; ++i)
      if (f->data[i].op == NYIR_STORE_LOCAL && f->data[i].imm == da->imm)
        return false;
    return true;
  }

  int av = -1, bv = -1;
  if (!nyr_guard_unique_local_store(f, da->imm, &av) ||
      !nyr_guard_unique_local_store(f, db->imm, &bv))
    return false;
  av = nyr_value_skip_copies(f, defs, av);
  bv = nyr_value_skip_copies(f, defs, bv);
  return (av >= 0 && av == bv) || nyr_values_same_at(f, defs, av, bv, at);
}

static bool nyr_guard_load_last_store_value(const nyr_func_t *f,
                                             const int *defs, int value,
                                             int *stored_out) {
  if (!f || !defs || !stored_out)
    return false;
  value = nyr_value_skip_copies(f, defs, value);
  if (value < 0 || value >= f->next_value)
    return false;
  int load_i = defs[value];
  if (load_i < 0 || (size_t)load_i >= f->len)
    return false;
  const nyr_inst_t *load = &f->data[load_i];
  if (load->op != NYIR_LOAD_LOCAL || load->imm < 0 ||
      nyr_guard_local_address_taken(f, load->imm))
    return false;

  size_t store_i = SIZE_MAX;
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->op == NYIR_STORE_LOCAL && in->imm == load->imm)
      store_i = i;
  }
  /*
   * The last store in the whole function cannot be changed by a loop backedge.
   * Requiring it to dominate this load makes crossing labels/calls safe; an
   * address-taken slot was rejected above, so calls cannot mutate it either.
   */
  if (store_i == SIZE_MAX || store_i >= (size_t)load_i) {
    if (ny_trace_enabled("NY_TRACE_BCE"))
      ny_trace_line(
          "BCE",
          "bce: last-store miss load=v%d slot=%lld load_i=%d store_i=%zu",
          value, (long long)load->imm, load_i, store_i);
    return false;
  }
  nyr_cfg_t cfg = {0};
  if (!nyr_cfg_build(f, &cfg))
    return false;
  bool dominates = nyr_cfg_dominates(&cfg, cfg.inst_block[store_i],
                                      cfg.inst_block[(size_t)load_i]);
  if (ny_trace_enabled("NY_TRACE_BCE"))
    ny_trace_line("BCE",
                  "bce: last-store load=v%d slot=%lld load_i=%d store_i=%zu "
                  "dom=%d src=v%d",
                  value, (long long)load->imm, load_i, store_i,
                  dominates ? 1 : 0, f->data[store_i].a);
  nyr_cfg_free(&cfg);
  if (!dominates)
    return false;
  *stored_out = f->data[store_i].a;
  return *stored_out >= 0;
}

static bool nyr_guard_values_same_at_depth(const nyr_func_t *f,
                                            const int *defs, int a, int b,
                                            size_t at, unsigned depth) {
  if (!f || !defs || depth > 12)
    return false;
  a = nyr_value_skip_copies(f, defs, a);
  b = nyr_value_skip_copies(f, defs, b);
  if (nyr_values_same_at(f, defs, a, b, at) ||
      nyr_guard_loads_same_local_value(f, defs, a, b, at))
    return true;

  int stored = -1;
  if (nyr_guard_load_last_store_value(f, defs, a, &stored) && stored != a &&
      nyr_guard_values_same_at_depth(f, defs, stored, b, at, depth + 1))
    return true;
  if (nyr_guard_load_last_store_value(f, defs, b, &stored) && stored != b &&
      nyr_guard_values_same_at_depth(f, defs, a, stored, at, depth + 1))
    return true;

  if (a < 0 || b < 0 || a >= f->next_value || b >= f->next_value)
    return false;
  int ai = defs[a], bi = defs[b];
  if (ai < 0 || bi < 0 || (size_t)ai >= f->len || (size_t)bi >= f->len)
    return false;
  const nyr_inst_t *da = &f->data[ai];
  const nyr_inst_t *db = &f->data[bi];
  if (da->op != db->op)
    return false;
  switch (da->op) {
  case NYIR_CONST_I64:
    return da->imm == db->imm;
  case NYIR_ADD_I64:
  case NYIR_MUL_I64:
  case NYIR_AND_I64:
  case NYIR_OR_I64:
  case NYIR_XOR_I64:
    return (nyr_guard_values_same_at_depth(f, defs, da->a, db->a, at,
                                            depth + 1) &&
            nyr_guard_values_same_at_depth(f, defs, da->b, db->b, at,
                                            depth + 1)) ||
           (nyr_guard_values_same_at_depth(f, defs, da->a, db->b, at,
                                            depth + 1) &&
            nyr_guard_values_same_at_depth(f, defs, da->b, db->a, at,
                                            depth + 1));
  case NYIR_SUB_I64:
  case NYIR_SHL_I64:
  case NYIR_SAR_I64:
    return nyr_guard_values_same_at_depth(f, defs, da->a, db->a, at,
                                           depth + 1) &&
           nyr_guard_values_same_at_depth(f, defs, da->b, db->b, at,
                                           depth + 1);
  default:
    return false;
  }
}

static bool nyr_guard_values_same_at(const nyr_func_t *f, const int *defs,
                                      int a, int b, size_t at) {
  return nyr_guard_values_same_at_depth(f, defs, a, b, at, 0);
}

static bool nyr_value_is_minus_one_from(const nyr_func_t *f, const int *defs,
                                         int value, int base, size_t at) {
  value = nyr_value_skip_copies(f, defs, value);
  base = nyr_value_skip_copies(f, defs, base);
  if (!f || !defs || value < 0 || value >= f->next_value)
    return false;
  int def_i = defs[value];
  if (def_i < 0)
    return false;
  const nyr_inst_t *def = &f->data[def_i];
  int64_t c = 0;
  if (def->op == NYIR_SUB_I64 &&
      nyr_guard_values_same_at(f, defs, def->a, base, at) &&
      nyr_const_i64_value(f, defs, def->b, &c) && c == 1)
    return true;
  if (def->op == NYIR_ADD_I64 &&
      nyr_guard_values_same_at(f, defs, def->a, base, at) &&
      nyr_const_i64_value(f, defs, def->b, &c) && c == -1)
    return true;
  if (def->op == NYIR_ADD_I64 &&
      nyr_guard_values_same_at(f, defs, def->b, base, at) &&
      nyr_const_i64_value(f, defs, def->a, &c) && c == -1)
    return true;
  return false;
}

static size_t nyr_previous_non_nop(const nyr_func_t *f, size_t i) {
  while (i > 0) {
    --i;
    if (f->data[i].op != NYIR_NOP)
      return i;
  }
  return SIZE_MAX;
}

static size_t nyr_label_pred_count(const nyr_func_t *f, int label,
                                    size_t label_i) {
  size_t count = 0;
  for (size_t i = 0; f && i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if ((in->op == NYIR_BR || in->op == NYIR_BR_IF) && in->imm == label)
      ++count;
  }
  size_t prev = nyr_previous_non_nop(f, label_i);
  if (prev != SIZE_MAX) {
    nyr_op_t op = f->data[prev].op;
    if (op != NYIR_BR && op != NYIR_RET)
      ++count;
  }
  return count;
}

static bool nyr_cmp_proves_index_in_len(const nyr_func_t *f, const int *defs,
                                         const nyr_inst_t *cmp, bool true_edge,
                                         int index, int len, size_t at) {
  if (!cmp || cmp->op != NYIR_CMP_I64)
    return false;
  int lhs = nyr_value_skip_copies(f, defs, cmp->a);
  int rhs = nyr_value_skip_copies(f, defs, cmp->b);
  index = nyr_value_skip_copies(f, defs, index);
  len = nyr_value_skip_copies(f, defs, len);
  bool lhs_same = nyr_guard_values_same_at(f, defs, lhs, index, at);
  bool rhs_same = nyr_guard_values_same_at(f, defs, rhs, len, at);
  if (ny_trace_enabled("NY_TRACE_BCE"))
    ny_trace_line("BCE",
                  "bce: cmp-proof lhs=v%d index=v%d same=%d rhs=v%d len=v%d "
                  "same=%d edge=%d cmp=%d",
                  lhs, index, lhs_same ? 1 : 0, rhs, len, rhs_same ? 1 : 0,
                  true_edge ? 1 : 0, (int)cmp->cmp);
  if (!lhs_same)
    return false;
  if (true_edge) {
    if (cmp->cmp == NYIR_CMP_LT && rhs_same)
      return true;
    return cmp->cmp == NYIR_CMP_LE &&
           nyr_value_is_minus_one_from(f, defs, rhs, len, at);
  }
  if (cmp->cmp == NYIR_CMP_GE &&
      nyr_guard_values_same_at(f, defs, rhs, len, at))
    return true;
  return cmp->cmp == NYIR_CMP_GT &&
         nyr_value_is_minus_one_from(f, defs, rhs, len, at);
}

static const nyr_inst_t *nyr_branch_cmp(const nyr_func_t *f, const int *defs,
                                          const nyr_inst_t *br) {
  if (!f || !defs || !br || br->op != NYIR_BR_IF)
    return NULL;
  int cond = nyr_value_skip_copies(f, defs, br->a);
  if (cond < 0 || cond >= f->next_value)
    return NULL;
  int def_i = defs[cond];
  return def_i >= 0 ? &f->data[def_i] : NULL;
}

static bool nyr_guard_local_load_is_slot(const nyr_func_t *f, const int *defs,
                                          int value, int64_t slot) {
  if (!f || !defs || value < 0 || value >= f->next_value)
    return false;
  value = nyr_value_skip_copies(f, defs, value);
  if (value < 0 || value >= f->next_value)
    return false;
  int di = defs[value];
  return di >= 0 && (size_t)di < f->len && f->data[di].op == NYIR_LOAD_LOCAL &&
         f->data[di].imm == slot;
}

/*
 * Prove a lowered local is non-negative without relying on PHI-form SCEV.
 * This deliberately accepts only a simple induction discipline: the first
 * store seeds the slot with a non-negative constant, and every later store
 * either resets it to a non-negative constant or adds a non-negative constant
 * to a load of the same, non-address-taken slot.  This covers canonical
 * source counters while failing closed for subtraction, arbitrary assignment,
 * pointer aliases, and call-visible locals.
 */
static bool nyr_guard_local_value_nonnegative(const nyr_func_t *f,
                                               const int *defs, int value) {
  if (!f || !defs || value < 0 || value >= f->next_value)
    return false;
  value = nyr_value_skip_copies(f, defs, value);
  int di = value >= 0 && value < f->next_value ? defs[value] : -1;
  if (di < 0 || (size_t)di >= f->len)
    return false;
  const nyr_inst_t *load = &f->data[di];
  if (load->op != NYIR_LOAD_LOCAL || load->imm < 0 ||
      nyr_guard_local_address_taken(f, load->imm))
    return false;
  const int64_t slot = load->imm;
  bool saw_seed = false;
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *st = &f->data[i];
    if (st->op != NYIR_STORE_LOCAL || st->imm != slot)
      continue;
    int src = nyr_value_skip_copies(f, defs, st->a);
    int64_t c = 0;
    if (nyr_const_i64_value(f, defs, src, &c)) {
      if (c < 0)
        return false;
      saw_seed = true;
      continue;
    }
    if (!saw_seed || src < 0 || src >= f->next_value)
      return false;
    int si = defs[src];
    if (si < 0 || (size_t)si >= f->len)
      return false;
    const nyr_inst_t *add = &f->data[si];
    if (add->op != NYIR_ADD_I64)
      return false;
    int other = -1;
    if (nyr_guard_local_load_is_slot(f, defs, add->a, slot))
      other = add->b;
    else if (nyr_guard_local_load_is_slot(f, defs, add->b, slot))
      other = add->a;
    else
      return false;
    if (!nyr_const_i64_value(f, defs, other, &c) || c < 0)
      return false;
  }
  return saw_seed;
}

/*
 * Prove a local nonnegative without relying on arithmetic range assumptions:
 * every store must be a nonnegative constant and the slot must not escape.
 */
static bool nyr_guard_local_constant_nonnegative(const nyr_func_t *f,
                                                  const int *defs, int value) {
  if (!f || !defs || value < 0 || value >= f->next_value)
    return false;
  value = nyr_value_skip_copies(f, defs, value);
  int di = defs[value];
  if (di < 0 || (size_t)di >= f->len)
    return false;
  const nyr_inst_t *load = &f->data[di];
  if (load->op != NYIR_LOAD_LOCAL || load->imm < 0 ||
      nyr_guard_local_address_taken(f, load->imm))
    return false;
  bool saw_store = false;
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *store = &f->data[i];
    if (store->op != NYIR_STORE_LOCAL || store->imm != load->imm)
      continue;
    int64_t stored = 0;
    if (!nyr_const_i64_value(f, defs, store->a, &stored) || stored < 0)
      return false;
    saw_store = true;
  }
  return saw_store;
}

static bool nyr_cmp_proves_reverse_index_in_len(
    const nyr_func_t *f, const int *defs, const nyr_scev_info_t *scev,
    const nyr_cfg_t *cfg, const nyr_inst_t *cmp, bool true_edge, int index,
    int len, size_t at) {
  if (!f || !defs || !scev || !cfg || !cmp || cmp->op != NYIR_CMP_I64 ||
      !true_edge || cmp->cmp != NYIR_CMP_LT)
    return false;
  int iv = nyr_value_skip_copies(f, defs, cmp->a);
  int guard_len = nyr_value_skip_copies(f, defs, cmp->b);
  index = nyr_value_skip_copies(f, defs, index);
  len = nyr_value_skip_copies(f, defs, len);
  bool same_len = nyr_guard_values_same_at(f, defs, guard_len, len, at);
  if (!same_len || index < 0 || index >= f->next_value)
    return false;
  int di = defs[index];
  if (di < 0 || (size_t)di >= f->len)
    return false;
  const nyr_inst_t *sub = &f->data[di];
  bool same_iv = sub->op == NYIR_SUB_I64 &&
                 nyr_guard_values_same_at(f, defs, sub->b, iv, at);
  bool minus_one = sub->op == NYIR_SUB_I64 &&
                   nyr_value_is_minus_one_from(f, defs, sub->a, len, at);
  if (ny_trace_enabled("NY_TRACE_BCE"))
    ny_trace_line("BCE",
                  "bce: reverse-shape index=v%d iv=v%d len=v%d op=%d "
                  "same_len=%d same_iv=%d minus1=%d",
                  index, iv, len, (int)sub->op, same_len ? 1 : 0,
                  same_iv ? 1 : 0, minus_one ? 1 : 0);
  if (!same_iv || !minus_one)
    return false;

  /*
   * i < len proves len-1-i >= 0.  The upper half additionally needs i >= 0;
   * counted-loop SCEV provides exactly that fact without assuming source-level
   * signedness or silently weakening the runtime check.
   */
  nyr_scev_affine_t iv_affine = {0};
  bool affine_ok =
      nyr_scev_affine_at_cfg(f, scev, defs, cfg, iv, at, &iv_affine);
  if (affine_ok && iv_affine.range.has_min && iv_affine.range.min >= 0)
    return true;
  return nyr_guard_local_value_nonnegative(f, defs, iv);
}

static bool nyr_block_for_label_local(const nyr_cfg_t *cfg, int64_t label,
                                       size_t *block_out) {
  if (!cfg)
    return false;
  for (size_t b = 0; b < cfg->block_count; ++b) {
    if (cfg->block_label[b] != label)
      continue;
    if (block_out)
      *block_out = b;
    return true;
  }
  return false;
}

static bool nyr_cmp_edge_proves_nonnegative(const nyr_func_t *f,
                                             const int *defs,
                                             const nyr_inst_t *cmp,
                                             bool true_edge, int value,
                                             size_t at) {
  if (!f || !defs || !cmp || cmp->op != NYIR_CMP_I64 || cmp->a < 0 ||
      cmp->b < 0 || !nyr_guard_values_same_at(f, defs, cmp->a, value, at))
    return false;
  int64_t rhs = 0;
  if (!nyr_const_i64_value(f, defs, cmp->b, &rhs))
    return false;
  if (true_edge)
    return (cmp->cmp == NYIR_CMP_GE && rhs == 0) ||
           (cmp->cmp == NYIR_CMP_GT && rhs == -1);
  return (cmp->cmp == NYIR_CMP_LT && rhs == 0) ||
         (cmp->cmp == NYIR_CMP_LE && rhs == -1);
}

static bool nyr_cfg_single_pred_from(const nyr_cfg_t *cfg, size_t block,
                                      size_t pred) {
  return cfg && block < cfg->block_count && pred < cfg->block_count &&
         cfg->pred_offsets[block + 1] - cfg->pred_offsets[block] == 1 &&
         cfg->pred_blocks[cfg->pred_offsets[block]] == pred;
}

/*
 * A branch edge that dominates the select may establish its source index is
 * nonnegative even when the mutable local can be negative on other paths.
 */
static bool nyr_guarded_index_nonnegative(const nyr_func_t *f,
                                           const int *defs, int value,
                                           size_t at) {
  if (!f || !defs || at >= f->len)
    return false;
  nyr_cfg_t cfg = {0};
  if (!nyr_cfg_build(f, &cfg))
    return false;
  size_t use_block = cfg.inst_block[at];
  bool proven = false;
  for (size_t i = 0; i < f->len && !proven; ++i) {
    const nyr_inst_t *br = &f->data[i];
    if (br->op != NYIR_BR_IF || br->a < 0 || br->a >= f->next_value)
      continue;
    int condition = nyr_value_skip_copies(f, defs, br->a);
    int cmp_i =
        condition >= 0 && condition < f->next_value ? defs[condition] : -1;
    if (cmp_i < 0 || (size_t)cmp_i >= f->len)
      continue;
    const nyr_inst_t *cmp = &f->data[cmp_i];
    size_t branch_block = cfg.inst_block[i];
    size_t true_block = 0;
    if (nyr_cmp_edge_proves_nonnegative(f, defs, cmp, true, value, at) &&
        nyr_block_for_label_local(&cfg, br->imm, &true_block) &&
        nyr_cfg_single_pred_from(&cfg, true_block, branch_block) &&
        nyr_cfg_dominates(&cfg, true_block, use_block)) {
      proven = true;
      break;
    }
    if (!nyr_cmp_edge_proves_nonnegative(f, defs, cmp, false, value, at))
      continue;
    size_t target_block = 0;
    if (!nyr_block_for_label_local(&cfg, br->imm, &target_block) ||
        branch_block >= cfg.block_count ||
        cfg.succ_offsets[branch_block + 1] - cfg.succ_offsets[branch_block] !=
            2)
      continue;
    size_t false_block = SIZE_MAX;
    for (size_t e = cfg.succ_offsets[branch_block];
         e < cfg.succ_offsets[branch_block + 1]; ++e)
      if (cfg.succ_blocks[e] != target_block)
        false_block = cfg.succ_blocks[e];
    if (false_block != SIZE_MAX &&
        nyr_cfg_single_pred_from(&cfg, false_block, branch_block) &&
        nyr_cfg_dominates(&cfg, false_block, use_block))
      proven = true;
  }
  nyr_cfg_free(&cfg);
  return proven;
}

static bool nyr_guard_proves_index_in_len(const nyr_func_t *f,
                                           const int *defs, size_t label_i,
                                           int label, int index, int len,
                                           size_t at) {
  if (nyr_label_pred_count(f, label, label_i) != 1)
    return false;
  size_t prev = nyr_previous_non_nop(f, label_i);
  if (prev != SIZE_MAX) {
    const nyr_inst_t *br = &f->data[prev];
    if (br->op == NYIR_BR_IF && br->imm != label) {
      const nyr_inst_t *cmp = nyr_branch_cmp(f, defs, br);
      if (nyr_cmp_proves_index_in_len(f, defs, cmp, false, index, len, at))
        return true;
    }
  }
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *br = &f->data[i];
    if (br->op != NYIR_BR_IF || br->imm != label)
      continue;
    const nyr_inst_t *cmp = nyr_branch_cmp(f, defs, br);
    return nyr_cmp_proves_index_in_len(f, defs, cmp, true, index, len, at);
  }
  return false;
}

/*
 * Return the logical buffer lineage which may own a tbuf pointer.
 * Constructors establish roots; list mutations, copies, PHIs, and
 * non-address-taken locals preserve them only when every incoming value
 * resolves to the same root. Mutation results preserve only this may-alias
 * lineage: append/set can reallocate, detach, or fail, so seeing one in the
 * length-forwarding scan still rejects the proof. Effect-proven nonmutating
 * borrows preserve the stronger stable-pointer case.
 */
enum { NYIR_BCE_TBUF_ROOT_CYCLE = -2 };

typedef struct {
  const nyr_func_t *f;
  const int *defs;
  const nyr_cfg_t *cfg;
  int *roots;
  uint8_t *state;
  uint8_t *blocks_reaching_load;
  size_t *reach_queue;
  size_t reach_capacity;
  size_t reach_load_i;
  unsigned root_trace_remaining;
} nyr_bce_tbuf_roots_t;

static int nyr_bce_tbuf_root_value(nyr_bce_tbuf_roots_t *roots, int value);

/*
 * A local load is only affected by stores in blocks that can reach that
 * load. Including a store from a later, unrelated loop made otherwise
 * uniform buffer locals look like unknown aliases. This is a CFG
 * over-approximation: it does not attempt to kill stores on intervening
 * paths, so it may reject a root but cannot omit a reaching mutation.
 */
static bool nyr_bce_local_store_can_reach_load(nyr_bce_tbuf_roots_t *roots,
                                                size_t store_i, size_t load_i) {
  if (!roots || !roots->cfg || store_i >= roots->f->len ||
      load_i >= roots->f->len || !roots->cfg->inst_block)
    return true;
  const nyr_cfg_t *cfg = roots->cfg;
  size_t store_block = cfg->inst_block[store_i];
  size_t load_block = cfg->inst_block[load_i];
  if (store_block >= cfg->block_count || load_block >= cfg->block_count)
    return true;
  if (!cfg->reachable[store_block])
    return false;
  if (store_block == load_block)
    return true;
  if (!roots->blocks_reaching_load || !roots->reach_queue) {
    roots->blocks_reaching_load =
        calloc(cfg->block_count, sizeof(*roots->blocks_reaching_load));
    roots->reach_queue = malloc(cfg->block_count * sizeof(*roots->reach_queue));
    if (!roots->blocks_reaching_load || !roots->reach_queue) {
      free(roots->blocks_reaching_load);
      free(roots->reach_queue);
      roots->blocks_reaching_load = NULL;
      roots->reach_queue = NULL;
      roots->reach_capacity = 0;
      return true;
    }
    roots->reach_capacity = cfg->block_count;
  }
  if (roots->reach_capacity != cfg->block_count)
    return true;
  if (roots->reach_load_i == load_i)
    return roots->blocks_reaching_load[store_block] != 0;

  memset(roots->blocks_reaching_load, 0,
         cfg->block_count * sizeof(*roots->blocks_reaching_load));
  size_t head = 0;
  size_t tail = 0;
  roots->blocks_reaching_load[load_block] = 1;
  roots->reach_queue[tail++] = load_block;
  while (head < tail) {
    size_t block = roots->reach_queue[head++];
    for (size_t e = cfg->pred_offsets[block]; e < cfg->pred_offsets[block + 1];
         ++e) {
      size_t pred = cfg->pred_blocks[e];
      if (!cfg->reachable[pred] || roots->blocks_reaching_load[pred])
        continue;
      roots->blocks_reaching_load[pred] = 1;
      roots->reach_queue[tail++] = pred;
    }
  }
  roots->reach_load_i = load_i;
  return roots->blocks_reaching_load[store_block] != 0;
}

static int nyr_bce_return_borrow_arg(const nyr_inst_t *in) {
  int args[NYIR_CALL_MAX_ARGS];
  int argc = 0;
  if (!in ||
      !nyr_call_args(in, INT_MAX, args, NYIR_CALL_MAX_ARGS, &argc, NULL, 0))
    return -1;
  int borrowed = -1;
  for (int i = 0; i < argc; ++i) {
    if (nyr_call_arg_contract_get(in, (size_t)i) !=
        NYIR_ARG_CONTRACT_RETURNS_BORROW)
      continue;
    if (borrowed >= 0)
      return -1;
    borrowed = args[i];
  }
  return borrowed;
}

static bool nyr_bce_call_has_nonmutating_borrow(const nyr_inst_t *in) {
  if (!in || (in->flags & NYIR_INST_F_EFFECTS_KNOWN) == 0 ||
      (in->effects & NYIR_EFFECT_CALL) == 0 ||
      (in->effects &
       (NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_UNKNOWN_SIDE_EFFECT)) != 0)
    return false;
  return nyr_bce_return_borrow_arg(in) >= 0;
}

static bool nyr_bce_call_cannot_mutate_memory(const nyr_inst_t *in) {
  if (!in || in->op != NYIR_CALL)
    return false;
  unsigned effects = nyr_effective_effects(in);
  return (effects & NYIR_EFFECT_CALL) != 0 &&
         (effects &
          (NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_UNKNOWN_SIDE_EFFECT |
           NYIR_EFFECT_FFI | NYIR_EFFECT_THREAD | NYIR_EFFECT_VOLATILE)) == 0;
}

static bool nyr_bce_call_is_nonmutating_borrow(const nyr_inst_t *in, int root,
                                                nyr_bce_tbuf_roots_t *roots) {
  int arg = nyr_bce_return_borrow_arg(in);
  return nyr_bce_call_has_nonmutating_borrow(in) && roots && arg >= 0 &&
         nyr_bce_tbuf_root_value(roots, arg) == root;
}

static int nyr_bce_tbuf_root(nyr_bce_tbuf_roots_t *roots, int value,
                              unsigned depth);

static int nyr_bce_tbuf_root_uncached(nyr_bce_tbuf_roots_t *roots, int value,
                                       unsigned depth) {
  const nyr_func_t *f = roots->f;
  const int *defs = roots->defs;
  if (depth > 32)
    return -1;
  int di = defs[value];
  if (di < 0 || (size_t)di >= f->len)
    return -1;
  const nyr_inst_t *in = &f->data[di];
  if (in->op == NYIR_CALL) {
    if (in->symbol && (strcmp(in->symbol, "rt_tbuf_new_raw") == 0 ||
                       strcmp(in->symbol, "rt_list_new_raw") == 0 ||
                       strcmp(in->symbol, "i64buf_new") == 0 ||
                       strcmp(in->symbol, "f64buf_new") == 0 ||
                       strcmp(in->symbol, "f32buf_new") == 0))
      return value;
    if (in->symbol && strcmp(in->symbol, "rt_list_set_len_raw") == 0)
      return nyr_bce_tbuf_root(roots, in->a, depth + 1);
    if (in->symbol && (strcmp(in->symbol, "rt_tbuf_append_raw") == 0 ||
                       strcmp(in->symbol, "rt_tbuf_append_i64_raw") == 0 ||
                       strcmp(in->symbol, "rt_tbuf_set_i64_raw") == 0))
      return nyr_bce_tbuf_root(roots, in->a, depth + 1);
    /*
     * Some typed-buffer paths cross the generic value-to-integer bridge
     * after append.  rt_any_to_i64 returns a recognized tbuf handle unchanged
     * (see its rt_tbuf_check path); when the IR also marks this result as a
     * typed buffer, preserve the input's allocation lineage.  The scalar
     * class guard is essential: ordinary integer unboxing must not inherit
     * pointer provenance.
     */
    if (in->symbol && strcmp(in->symbol, "rt_any_to_i64") == 0 && in->a >= 0 &&
        in->scalar_class == NYIR_SCALAR_TYPED_BUFFER)
      return nyr_bce_tbuf_root(roots, in->a, depth + 1);
    int borrowed = nyr_bce_return_borrow_arg(in);
    if (borrowed >= 0 && nyr_bce_call_has_nonmutating_borrow(in))
      return nyr_bce_tbuf_root(roots, borrowed, depth + 1);
  }
  if (in->op == NYIR_COPY && in->a >= 0)
    return nyr_bce_tbuf_root(roots, in->a, depth + 1);
  if (in->op == NYIR_LOAD_LOCAL && in->imm >= 0) {
    /*
     * A typed-buffer formal is the allocation root for the function. It has
     * no preceding STORE_LOCAL, so the ordinary local-store walk below can
     * never discover its provenance. Only treat the formal as a unique root
     * when the function has a single typed-buffer parameter; distinct formals
     * may alias at a call site.
     */
    if ((size_t)in->imm < f->param_count &&
        in->scalar_class == NYIR_SCALAR_TYPED_BUFFER) {
      size_t tbuf_params = 0;
      for (size_t i = 0; i < f->len; ++i) {
        const nyr_inst_t *param = &f->data[i];
        if (param->op == NYIR_LOAD_LOCAL && param->imm >= 0 &&
            (size_t)param->imm < f->param_count &&
            param->scalar_class == NYIR_SCALAR_TYPED_BUFFER) {
          bool seen = false;
          for (size_t j = 0; j < i; ++j)
            if (f->data[j].op == NYIR_LOAD_LOCAL &&
                f->data[j].imm == param->imm &&
                f->data[j].scalar_class == NYIR_SCALAR_TYPED_BUFFER) {
              seen = true;
              break;
            }
          tbuf_params += !seen;
        }
      }
      if (tbuf_params == 1)
        return value;
    }
    if (nyr_guard_local_address_taken(f, in->imm)) {
      if (roots->root_trace_remaining > 0 && ny_trace_enabled("NY_TRACE_BCE")) {
        --roots->root_trace_remaining;
        ny_trace_line("BCE",
                      "bce: tbuf-root reject v%d load=%d slot=%lld: "
                      "address-taken local",
                      value, di, (long long)in->imm);
      }
      return -1;
    }
    int root = -1;
    int conflicting_store = -1;
    int conflicting_root = -1;
    bool saw_cycle = false;
    bool same = true;
    for (size_t i = 0; i < f->len; ++i) {
      const nyr_inst_t *store = &f->data[i];
      if (store->op != NYIR_STORE_LOCAL || store->imm != in->imm)
        continue;
      if (!nyr_bce_local_store_can_reach_load(roots, i, (size_t)di))
        continue;
      int next = nyr_bce_tbuf_root(roots, store->a, depth + 1);
      if (next == NYIR_BCE_TBUF_ROOT_CYCLE) {
        saw_cycle = true;
        continue;
      }
      if (next < 0 || (root >= 0 && root != next)) {
        same = false;
        conflicting_store = (int)i;
        conflicting_root = next;
        break;
      }
      root = next;
    }
    if (!same) {
      if (roots->root_trace_remaining > 0 && ny_trace_enabled("NY_TRACE_BCE")) {
        --roots->root_trace_remaining;
        ny_trace_line("BCE",
                      "bce: tbuf-root reject v%d load=%d slot=%lld "
                      "store=%d root=v%d: local stores disagree",
                      value, di, (long long)in->imm, conflicting_store,
                      conflicting_root);
      }
      return -1;
    }
    if (root < 0 && !saw_cycle && roots->root_trace_remaining > 0 &&
        ny_trace_enabled("NY_TRACE_BCE")) {
      --roots->root_trace_remaining;
      ny_trace_line("BCE",
                    "bce: tbuf-root reject v%d load=%d slot=%lld: "
                    "no reaching buffer root",
                    value, di, (long long)in->imm);
    }
    return root >= 0 ? root : saw_cycle ? NYIR_BCE_TBUF_ROOT_CYCLE : -1;
  }
  if (in->op == NYIR_PHI && in->phi_incoming_len) {
    int root = -1;
    int conflicting_value = -1;
    int conflicting_root = -1;
    bool same = true;
    bool saw_cycle = false;
    for (size_t i = 0; i < in->phi_incoming_len; ++i) {
      int next =
          nyr_bce_tbuf_root(roots, in->phi_incoming[i].value, depth + 1);
      if (next == NYIR_BCE_TBUF_ROOT_CYCLE) {
        saw_cycle = true;
        continue;
      }
      if (next < 0 || (root >= 0 && root != next)) {
        same = false;
        conflicting_value = in->phi_incoming[i].value;
        conflicting_root = next;
        break;
      }
      root = next;
    }
    if (!same) {
      if (roots->root_trace_remaining > 0 && ny_trace_enabled("NY_TRACE_BCE")) {
        --roots->root_trace_remaining;
        ny_trace_line("BCE",
                      "bce: tbuf-root reject v%d phi=%d incoming=v%d "
                      "root=v%d: incoming roots disagree",
                      value, di, conflicting_value, conflicting_root);
      }
      return -1;
    }
    return root >= 0 ? root : saw_cycle ? NYIR_BCE_TBUF_ROOT_CYCLE : -1;
  }
  if (in->op == NYIR_ADD_I64 || in->op == NYIR_SUB_I64) {
    int left = nyr_bce_tbuf_root(roots, in->a, depth + 1);
    int right = nyr_bce_tbuf_root(roots, in->b, depth + 1);
    return left >= 0 && right == -1   ? left
           : right >= 0 && left == -1 ? right
                                      : -1;
  }
  return -1;
}

static int nyr_bce_tbuf_root(nyr_bce_tbuf_roots_t *roots, int value,
                              unsigned depth) {
  if (!roots || !roots->f || !roots->defs || !roots->roots || !roots->state ||
      depth > 32)
    return -1;
  const nyr_func_t *f = roots->f;
  value = nyr_value_skip_copies(f, roots->defs, value);
  if (value < 0 || value >= f->next_value)
    return -1;
  if (roots->state[value] == 1)
    return NYIR_BCE_TBUF_ROOT_CYCLE;
  if (roots->state[value] == 2)
    return roots->roots[value];
  roots->state[value] = 1;
  int root = nyr_bce_tbuf_root_uncached(roots, value, depth);
  if (root == NYIR_BCE_TBUF_ROOT_CYCLE) {
    /*
     * A cycle result is contextual; do not cache it as a root fact.
     */
    roots->state[value] = 0;
    return root;
  }
  roots->roots[value] = root;
  roots->state[value] = 2;
  return root >= 0 ? root : -1;
}

static int nyr_bce_tbuf_root_value(nyr_bce_tbuf_roots_t *roots, int value) {
  return nyr_bce_tbuf_root(roots, value, 0);
}

static int nyr_bce_exit_value_limit(const nyr_func_t *f, const int *defs,
                                     const nyr_scev_info_t *scev,
                                     const nyr_cfg_t *cfg, int exit_value,
                                     int guarded_index, size_t at) {
  if (!f || !defs || !scev || !cfg || exit_value < 0 || guarded_index < 0 ||
      at >= f->len)
    return -1;
  exit_value = nyr_value_skip_copies(f, defs, exit_value);
  nyr_scev_affine_t index_range = {0};
  if (!nyr_scev_affine_at_cfg(f, scev, defs, cfg, guarded_index, at,
                               &index_range) ||
      !index_range.range.has_min || index_range.range.min < 0)
    return -1;
  size_t use_block = cfg->inst_block[at];
  for (size_t i = 0; i < scev->count; ++i) {
    const nyr_scev_loop_t *loop = &scev->loops[i];
    if (loop->iv != exit_value || !loop->init_is_const || loop->init != 0 ||
        loop->step != 1 || loop->predicate != NYIR_CMP_LT ||
        loop->limit_value < 0)
      continue;
    bool *members = calloc(cfg->block_count, sizeof(*members));
    if (!members)
      return -1;
    bool ok = nyr_cfg_natural_loop_blocks(
        cfg, loop->latch_block, loop->header_block, members, cfg->block_count);
    size_t exit_block = SIZE_MAX;
    for (size_t b = 0; ok && b < cfg->block_count; ++b) {
      if (!members[b])
        continue;
      for (size_t e = cfg->succ_offsets[b]; e < cfg->succ_offsets[b + 1]; ++e) {
        size_t succ = cfg->succ_blocks[e];
        if (members[succ])
          continue;
        if (exit_block != SIZE_MAX && exit_block != succ) {
          ok = false;
          break;
        }
        exit_block = succ;
      }
    }
    ok = ok && exit_block != SIZE_MAX && use_block < cfg->block_count &&
         !members[use_block] && nyr_cfg_dominates(cfg, exit_block, use_block);
    free(members);
    if (ok)
      return loop->limit_value;
  }
  return -1;
}

static bool nyr_bce_tbuf_header_offset(nyr_bce_tbuf_roots_t *roots,
                                        int address, int root,
                                        int64_t wanted_offset) {
  const nyr_func_t *f = roots->f;
  const int *defs = roots->defs;
  address = nyr_value_skip_copies(f, defs, address);
  if (address < 0 || address >= f->next_value)
    return false;
  int di = defs[address];
  if (di < 0 || (size_t)di >= f->len)
    return false;
  const nyr_inst_t *in = &f->data[di];
  if (in->op != NYIR_ADD_I64)
    return false;
  int64_t offset = 0;
  if (nyr_const_i64_value(f, defs, in->a, &offset) &&
      offset == wanted_offset && nyr_bce_tbuf_root_value(roots, in->b) == root)
    return true;
  return nyr_const_i64_value(f, defs, in->b, &offset) &&
         offset == wanted_offset &&
         nyr_bce_tbuf_root_value(roots, in->a) == root;
}

/*
 * Resolve a dynamic tbuf-header length load to the dominating
 * rt_list_set_len_raw value, but only when that setter dominates the header
 * load and no earlier mutation, alias, or escape can change the loaded value.
 * Calls and writes after the load cannot invalidate the scalar SSA value
 * consumed by the bounds check.
 */
static bool nyr_bce_tbuf_length_value(const nyr_func_t *f, const int *defs,
                                       const nyr_scev_info_t *scev, int base,
                                       int loaded_length, int guarded_index,
                                       size_t at, const nyr_cfg_t *cfg,
                                       nyr_bce_tbuf_roots_t *roots,
                                       int *length_out) {
  if (!cfg)
    return false;
  if (!f || !defs || !length_out || at >= f->len)
    return false;
  int root = nyr_bce_tbuf_root_value(roots, base);
  int length = nyr_value_skip_copies(f, defs, loaded_length);
  if (root < 0 || length < 0 || length >= f->next_value) {
    if (ny_trace_enabled("NY_TRACE_BCE"))
      ny_trace_line("BCE",
                    "bce: len-forward reject check=%zu base=v%d root=v%d "
                    "length=v%d: unknown tbuf provenance",
                    at, base, root, length);
    return false;
  }
  int load_i = defs[length];
  if (load_i < 0 || (size_t)load_i >= at ||
      f->data[load_i].op != NYIR_LOAD_I64 ||
      !nyr_bce_tbuf_header_offset(roots, f->data[load_i].a, root, -24)) {
    if (ny_trace_enabled("NY_TRACE_BCE"))
      ny_trace_line("BCE",
                    "bce: len-forward reject check=%zu length=v%d def=%d: not "
                    "a direct tbuf length-header load",
                    at, length, load_i);
    return false;
  }

  size_t use_block = cfg->inst_block[at];
  int selected_length = -1;
  size_t selected_store = SIZE_MAX;
  int alloc_i = defs[root];
  bool safe = alloc_i >= 0 && (size_t)alloc_i < (size_t)load_i;

  /*
   * Only writes and escapes before the header load can invalidate the scalar
   * value being forwarded. Later mutations cannot change an SSA load that
   * already executed; the bounds check must continue to consume that same
   * loaded value, so post-load effects do not invalidate this substitution.
   */
  for (size_t i = (size_t)(alloc_i >= 0 ? alloc_i + 1 : 0);
       safe && i < (size_t)load_i; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->op == NYIR_CALL) {
      bool reaches_root = false;
      const int args[] = {in->a, in->b, in->c, in->d, in->e, in->f};
      for (size_t k = 0; k < sizeof(args) / sizeof(args[0]); ++k)
        if (args[k] >= 0 && nyr_bce_tbuf_root_value(roots, args[k]) == root)
          reaches_root = true;
      for (size_t k = 0; !reaches_root && k < in->extra_args_len; ++k)
        if (in->extra_args[k] >= 0 &&
            nyr_bce_tbuf_root_value(roots, in->extra_args[k]) == root)
          reaches_root = true;
      if (reaches_root) {
        bool setter = in->symbol &&
                      strcmp(in->symbol, "rt_list_set_len_raw") == 0 &&
                      in->b >= 0 && i < (size_t)load_i &&
                      nyr_cfg_dominates(cfg, cfg->inst_block[i], use_block);
        bool pure_borrow = nyr_bce_call_is_nonmutating_borrow(in, root, roots);
        bool readonly_call = nyr_bce_call_cannot_mutate_memory(in);
        if (!setter && !pure_borrow && !readonly_call) {
          if (ny_trace_enabled("NY_TRACE_BCE"))
            ny_trace_line("BCE",
                          "bce: len-forward reject check=%zu call=%zu fn=%s: "
                          "buffer passed to a mutator or unproved borrow",
                          at, i, in->symbol ? in->symbol : "<indirect>");
          safe = false;
          break;
        }
        if (setter) {
          selected_length = in->b;
          selected_store = i;
        }
      }
    } else if (in->op == NYIR_STORE_I64) {
      if (in->c >= 0 && nyr_bce_tbuf_root_value(roots, in->c) == root) {
        if (ny_trace_enabled("NY_TRACE_BCE"))
          ny_trace_line("BCE",
                        "bce: len-forward reject check=%zu store=%zu: pointer "
                        "escaped into memory",
                        at, i);
        safe = false; /* pointer escaped into memory */
        break;
      }
      if (nyr_bce_tbuf_header_offset(roots, in->a, root, -24)) {
        if (ny_trace_enabled("NY_TRACE_BCE"))
          ny_trace_line("BCE",
                        "bce: len-forward reject check=%zu store=%zu: direct "
                        "length-header write",
                        at, i);
        safe = false; /* direct header write bypassed the tracked setter */
        break;
      }
    } else if (in->op == NYIR_STORE_LOCAL && in->a >= 0 &&
               nyr_bce_tbuf_root_value(roots, in->a) == root &&
               nyr_guard_local_address_taken(f, in->imm)) {
      safe = false; /* address-taken local can escape through its slot */
    } else if (in->op == NYIR_RET && in->a >= 0 &&
               nyr_bce_tbuf_root_value(roots, in->a) == root) {
      safe = false; /* caller-visible alias */
    }
  }

  if (safe && selected_length >= 0 && selected_store != SIZE_MAX &&
      selected_store < (size_t)load_i &&
      nyr_cfg_dominates(cfg, cfg->inst_block[selected_store],
                         cfg->inst_block[load_i])) {
    int loop_limit = nyr_bce_exit_value_limit(
        f, defs, scev, cfg, selected_length, guarded_index, at);
    *length_out = loop_limit >= 0 ? loop_limit : selected_length;
  } else {
    safe = false;
  }
  if (ny_trace_enabled("NY_TRACE_BCE"))
    ny_trace_line("BCE",
                  "bce: len-forward %s check=%zu base=v%d root=v%d header=%d "
                  "setter=%zu length=v%d",
                  safe ? "accept" : "reject", at, base, root, load_i,
                  selected_store, selected_length);
  return safe;
}

static bool
nyr_bce_call_is_nondecreasing_tbuf_mutation(const nyr_inst_t *in) {
  return in && in->op == NYIR_CALL && in->symbol &&
         (strcmp(in->symbol, "rt_tbuf_append_raw") == 0 ||
          strcmp(in->symbol, "rt_tbuf_append_i64_raw") == 0 ||
          strcmp(in->symbol, "rt_tbuf_set_i64_raw") == 0);
}

static bool nyr_bce_call_passes_root(const nyr_inst_t *in, int root,
                                      nyr_bce_tbuf_roots_t *roots) {
  if (!in || !roots || root < 0)
    return false;
  const int args[] = {in->a, in->b, in->c, in->d, in->e, in->f};
  for (size_t i = 0; i < sizeof(args) / sizeof(args[0]); ++i)
    if (args[i] >= 0 && nyr_bce_tbuf_root_value(roots, args[i]) == root)
      return true;
  for (size_t i = 0; i < in->extra_args_len; ++i)
    if (in->extra_args[i] >= 0 &&
        nyr_bce_tbuf_root_value(roots, in->extra_args[i]) == root)
      return true;
  return false;
}

/*
 * Verify that the checked tbuf's length cannot fall below a dominating
 * runtime lower-bound guard. Before that guard, arbitrary length changes are
 * allowed because the guard observes their result; afterward, only
 * read-only calls and known append/set operations preserve the lower bound.
 * Escapes, unknown writes, and direct header writes fail closed.
 */
static bool nyr_bce_tbuf_length_guard_stable(const nyr_func_t *f,
                                              const int *defs, int root,
                                              size_t guard_call_i,
                                              size_t check_i,
                                              nyr_bce_tbuf_roots_t *roots) {
  if (!f || !defs || !roots || root < 0 || guard_call_i >= check_i ||
      check_i >= f->len)
    return false;
  int root_i = defs[root];
  if (root_i < 0 || (size_t)root_i >= guard_call_i)
    return false;
  for (size_t i = (size_t)root_i + 1; i < check_i; ++i) {
    const nyr_inst_t *in = &f->data[i];
    bool after_guard = i > guard_call_i;
    if (in->op == NYIR_CALL) {
      /*
       * The guard observes the buffer after every preceding call has
       * completed, so pre-guard mutations cannot invalidate the lower bound
       * established by that guard.  In particular, append loops may contain
       * conservative or unknown helper calls while still producing a buffer
       * whose final length is checked immediately before the guarded region.
       */
      if (!after_guard)
        continue;
      bool passes_root = nyr_bce_call_passes_root(in, root, roots);
      unsigned effects = nyr_effective_effects(in);
      bool clock_read =
          in->symbol && (strcmp(in->symbol, "rt_monotonic_ns_raw") == 0 ||
                         strcmp(in->symbol, "rt_ticks_ns") == 0);
      bool readonly = nyr_bce_call_cannot_mutate_memory(in) || clock_read;
      bool set_length =
          in->symbol && strcmp(in->symbol, "rt_list_set_len_raw") == 0;
      if (passes_root) {
        if (nyr_bce_call_is_nondecreasing_tbuf_mutation(in) ||
            (set_length && !after_guard))
          continue;
        if (set_length)
          return false;
      }
      if (readonly)
        continue;
      if (effects &
          (NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_UNKNOWN_SIDE_EFFECT |
           NYIR_EFFECT_FFI | NYIR_EFFECT_THREAD | NYIR_EFFECT_VOLATILE)) {
        if (ny_trace_enabled("NY_TRACE_BCE"))
          ny_trace_line("BCE",
                        "bce: guard-stability reject check=%zu call=%zu "
                        "symbol=%s root=v%d passes-root=%d effects=0x%x",
                        check_i, i, in->symbol ? in->symbol : "<indirect>",
                        root, passes_root ? 1 : 0, effects);
        return false;
      }
    } else if (in->op == NYIR_STORE_I64) {
      if (in->c >= 0 && nyr_bce_tbuf_root_value(roots, in->c) == root) {
        if (ny_trace_enabled("NY_TRACE_BCE"))
          ny_trace_line("BCE",
                        "bce: guard-stability reject check=%zu store=%zu "
                        "root-address=v%d",
                        check_i, i, in->c);
        return false;
      }
      if (after_guard && nyr_bce_tbuf_header_offset(roots, in->a, root, -24)) {
        if (ny_trace_enabled("NY_TRACE_BCE"))
          ny_trace_line("BCE",
                        "bce: guard-stability reject check=%zu store=%zu "
                        "header-root=v%d",
                        check_i, i, root);
        return false;
      }
    } else if (in->op == NYIR_RET && in->a >= 0 &&
               nyr_bce_tbuf_root_value(roots, in->a) == root) {
      return false;
    }
  }
  return true;
}

static bool nyr_bce_trace_enabled(void);
static void nyr_bce_trace(const nyr_func_t *f, size_t pc, const char *status,
                           const char *reason);

static bool nyr_bce_cmp_lower_bound(nyr_cmp_t cmp, bool true_edge,
                                     int64_t constant, int64_t *minimum) {
  if (!minimum || constant < 0)
    return false;
  if (true_edge) {
    if (cmp == NYIR_CMP_GE || cmp == NYIR_CMP_EQ) {
      *minimum = constant;
      return true;
    }
    if (cmp == NYIR_CMP_GT && constant < INT64_MAX) {
      *minimum = constant + 1;
      return true;
    }
    return false;
  }
  if (cmp == NYIR_CMP_LT || cmp == NYIR_CMP_NE) {
    *minimum = constant;
    return true;
  }
  if (cmp == NYIR_CMP_LE && constant < INT64_MAX) {
    *minimum = constant + 1;
    return true;
  }
  return false;
}

static bool nyr_bce_tbuf_guarded_length_minimum(
    const nyr_func_t *f, const int *defs, const nyr_scev_info_t *scev,
    const nyr_cfg_t *cfg, nyr_bce_tbuf_roots_t *roots, int base, int index,
    size_t check_i, int64_t *minimum_out) {
  if (!f || !defs || !cfg || !roots || !minimum_out || check_i >= f->len)
    return false;
  int root = nyr_bce_tbuf_root_value(roots, base);
  if (root < 0) {
    if (ny_trace_enabled("NY_TRACE_BCE")) {
      int base_def = base >= 0 && base < f->next_value ? defs[base] : -1;
      const nyr_inst_t *base_in = base_def >= 0 && (size_t)base_def < f->len
                                       ? &f->data[base_def]
                                       : NULL;
      ny_trace_line("BCE",
                    "bce: length-guard reject check=%zu base=v%d def=%d "
                    "op=%d symbol=%s: unknown root",
                    check_i, base, base_def, base_in ? (int)base_in->op : -1,
                    base_in && base_in->symbol ? base_in->symbol : "<none>");
    }
    return false;
  }
  size_t check_block = cfg->inst_block[check_i];
  if (check_block >= cfg->block_count)
    return false;
  bool found = false;
  int64_t best_minimum = 0;
  for (size_t i = 0; i < check_i; ++i) {
    const nyr_inst_t *branch = &f->data[i];
    bool is_assertion = branch->op == NYIR_CALL && branch->symbol &&
                        strcmp(branch->symbol, "rt_assert_cstr") == 0;
    if (branch->op != NYIR_BR_IF && !is_assertion)
      continue;
    size_t branch_block = cfg->inst_block[i];
    if (branch_block >= cfg->block_count ||
        !nyr_cfg_dominates(cfg, branch_block, check_block) ||
        (is_assertion && branch_block == check_block && i >= check_i))
      continue;
    size_t target_block = 0;
    size_t succ_begin = 0;
    size_t succ_end = 0;
    if (!is_assertion) {
      if (!nyr_block_for_label_local(cfg, branch->imm, &target_block) ||
          target_block >= cfg->block_count)
        continue;
      succ_begin = cfg->succ_offsets[branch_block];
      succ_end = cfg->succ_offsets[branch_block + 1];
      if (succ_end - succ_begin != 2)
        continue;
    } else if (ny_trace_enabled("NY_TRACE_BCE")) {
      ny_trace_line("BCE", "bce: assert-guard candidate check=%zu call=%zu",
                    check_i, i);
    }

    nyr_inst_t assertion_condition = {.op = NYIR_BR_IF, .a = branch->a};
    const nyr_inst_t *cmp =
        nyr_branch_cmp(f, defs, is_assertion ? &assertion_condition : branch);
    if (!cmp || cmp->op != NYIR_CMP_I64) {
      if (ny_trace_enabled("NY_TRACE_BCE"))
        ny_trace_line("BCE",
                      "bce: length-guard reject check=%zu br=%zu: "
                      "condition is not an i64 comparison",
                      check_i, i);
      continue;
    }
    int len_value = -1;
    int64_t constant = 0;
    nyr_cmp_t relation = cmp->cmp;
    if (cmp->a >= 0 && nyr_const_i64_value(f, defs, cmp->b, &constant)) {
      len_value = nyr_value_skip_copies(f, defs, cmp->a);
    } else if (cmp->b >= 0 &&
               nyr_const_i64_value(f, defs, cmp->a, &constant)) {
      len_value = nyr_value_skip_copies(f, defs, cmp->b);
      relation = nyr_cmp_swap_operands(relation);
    } else {
      if (ny_trace_enabled("NY_TRACE_BCE"))
        ny_trace_line("BCE",
                      "bce: length-guard reject check=%zu br=%zu: "
                      "comparison has no constant operand",
                      check_i, i);
      continue;
    }
    if (len_value < 0 || len_value >= f->next_value)
      continue;
    int call_i = defs[len_value];
    if (call_i < 0 || (size_t)call_i >= i || (size_t)call_i >= f->len)
      continue;
    const nyr_inst_t *len_call = &f->data[call_i];
    int guarded_root = nyr_bce_tbuf_root_value(roots, len_call->a);
    /*
     * Both runtime entry points return logical element counts for a proven
     * typed-buffer root. The strict generic entry point is only trusted
     * after root analysis proves its argument is this same tbuf.
     */
    bool known_tbuf_length =
        len_call->op == NYIR_CALL && len_call->symbol &&
        (strcmp(len_call->symbol, "rt_tbuf_len_raw") == 0 ||
         strcmp(len_call->symbol, "rt_len_strict") == 0);
    if (!known_tbuf_length || guarded_root != root) {
      if (ny_trace_enabled("NY_TRACE_BCE"))
        ny_trace_line("BCE",
                      "bce: length-guard reject check=%zu br=%zu len-call=%s "
                      "arg=v%d root=v%d guarded-root=v%d op=%d imm=%lld "
                      "known=%d",
                      check_i, i,
                      len_call->symbol ? len_call->symbol : "<non-call>",
                      len_call->a, root, guarded_root, (int)len_call->op,
                      (long long)len_call->imm, known_tbuf_length ? 1 : 0);
      continue;
    }

    bool true_edge = true;
    if (!is_assertion) {
      bool target_reaches = nyr_bce_local_store_can_reach_load(
          roots, cfg->block_start[target_block], check_i);
      bool other_reaches = false;
      for (size_t edge = succ_begin; edge < succ_end; ++edge) {
        size_t successor = cfg->succ_blocks[edge];
        if (successor == target_block)
          continue;
        if (successor >= cfg->block_count)
          continue;
        other_reaches |= nyr_bce_local_store_can_reach_load(
            roots, cfg->block_start[successor], check_i);
      }
      if (target_reaches == other_reaches) {
        if (ny_trace_enabled("NY_TRACE_BCE"))
          ny_trace_line("BCE",
                        "bce: length-guard reject check=%zu br=%zu: "
                        "branch edges do not isolate the guarded path",
                        check_i, i);
        continue;
      }
      true_edge = target_reaches;
    }
    int64_t minimum = 0;
    bool cmp_bound =
        nyr_bce_cmp_lower_bound(relation, true_edge, constant, &minimum);
    bool stable = nyr_bce_tbuf_length_guard_stable(
        f, defs, root, (size_t)call_i, check_i, roots);
    if (!cmp_bound || !stable) {
      if (is_assertion && ny_trace_enabled("NY_TRACE_BCE"))
        ny_trace_line("BCE",
                      "bce: assert-guard reject check=%zu cmp-bound=%d "
                      "stable=%d min=%lld",
                      check_i, cmp_bound ? 1 : 0, stable ? 1 : 0,
                      (long long)minimum);
      continue;
    }

    nyr_scev_affine_t index_range = {0};
    int64_t index_min = 0;
    int64_t index_max = 0;
    bool ranged = index >= 0 &&
                  nyr_scev_affine_at_cfg(f, scev, defs, cfg, index, check_i,
                                          &index_range) &&
                  index_range.range.has_min && index_range.range.has_max;
    if (ranged) {
      index_min = index_range.range.min;
      index_max = index_range.range.max;
    } else if (!nyr_const_i64_value(f, defs, index, &index_min)) {
      if (nyr_bce_trace_enabled()) {
        char reason[160];
        snprintf(reason, sizeof(reason),
                 "runtime length guard lacks a bounded index range (v%d)",
                 index);
        nyr_bce_trace(f, check_i, "guard-reject", reason);
      }
      continue;
    } else {
      index_max = index_min;
    }
    if (index_min < 0 || index_max >= minimum) {
      if (nyr_bce_trace_enabled()) {
        char reason[192];
        snprintf(reason, sizeof(reason),
                 "runtime length guard index range [%lld,%lld] exceeds "
                 "minimum %lld",
                 (long long)index_min, (long long)index_max,
                 (long long)minimum);
        nyr_bce_trace(f, check_i, "guard-reject", reason);
      }
      continue;
    }
    if (ny_trace_enabled("NY_TRACE_BCE"))
      ny_trace_line("BCE",
                    "bce: length-guard accept check=%zu br=%zu root=v%d "
                    "edge=%s cmp=%d min=%lld index=[%lld,%lld]",
                    check_i, i, root, true_edge ? "true" : "false",
                    (int)relation, (long long)minimum, (long long)index_min,
                    (long long)index_max);
    if (!found || minimum > best_minimum)
      best_minimum = minimum;
    found = true;
  }
  if (found)
    *minimum_out = best_minimum;
  return found;
}

static bool nyr_bounds_check_guard_safe(const nyr_func_t *f, const int *defs,
                                         size_t check_i, size_t label_i,
                                         int label,
                                         const nyr_scev_info_t *scev,
                                         const nyr_cfg_t *cfg,
                                         nyr_bce_tbuf_roots_t *roots) {
  const nyr_inst_t *check = &f->data[check_i];
  if (ny_trace_enabled("NY_TRACE_BCE") && check->op == NYIR_BOUNDS_CHECK)
    ny_trace_line("BCE", "bce: guard-entry check=%zu b=v%d c=v%d imm=%lld",
                  check_i, check->b, check->c, (long long)check->imm);
  if (check->op != NYIR_BOUNDS_CHECK || check->c < 0)
    return false;
  int index = -1, len = -1;
  unsigned index_shift = 0, len_shift = 0;
  bool index_scaled = nyr_scaled_base(f, defs, check->b, &index, &index_shift);
  bool len_scaled = nyr_scaled_base(f, defs, check->c, &len, &len_shift);
  if (!index_scaled || !len_scaled || index_shift != len_shift) {
    if (ny_trace_enabled("NY_TRACE_BCE"))
      ny_trace_line("BCE",
                    "bce: guard reject check=%zu index-scaled=%d "
                    "len-scaled=%d index-shift=%u len-shift=%u",
                    check_i, index_scaled ? 1 : 0, len_scaled ? 1 : 0,
                    index_shift, len_shift);
    return false;
  }
  int forwarded_length = -1;
  if (nyr_bce_tbuf_length_value(f, defs, scev, check->a, len, index, check_i,
                                 cfg, roots, &forwarded_length))
    len = nyr_value_skip_copies(f, defs, forwarded_length);
  if (nyr_guard_proves_index_in_len(f, defs, label_i, label, index, len,
                                     check_i))
    return true;

  int64_t guarded_length_minimum = 0;
  if (nyr_bce_tbuf_guarded_length_minimum(f, defs, scev, cfg, roots, check->a,
                                           index, check_i,
                                           &guarded_length_minimum)) {
    if (ny_trace_enabled("NY_TRACE_BCE"))
      ny_trace_line("BCE",
                    "bce: runtime-length guard proves check=%zu index=v%d "
                    "length-at-least=%lld",
                    check_i, index, (long long)guarded_length_minimum);
    return true;
  }

  /*
   * Representation guards (elem-size/tag tests) commonly sit between a loop
   * guard and the actual load.  Walk all true branch targets that dominate the
   * check instead of requiring the loop comparison to be the immediate block
   * predecessor.  This remains CFG-proof based: a branch target must dominate
   * the check block, so every path reaching the check crossed that true edge.
   */
  if (!cfg)
    return false;
  size_t check_block = cfg->inst_block[check_i];
  bool safe = false;
  for (size_t i = 0; i < f->len && !safe; ++i) {
    const nyr_inst_t *br = &f->data[i];
    if (br->op != NYIR_BR_IF)
      continue;
    size_t target_block = 0;
    if (!nyr_block_for_label_local(cfg, br->imm, &target_block) ||
        !nyr_cfg_dominates(cfg, target_block, check_block))
      continue;
    const nyr_inst_t *cmp = nyr_branch_cmp(f, defs, br);
    bool direct =
        nyr_cmp_proves_index_in_len(f, defs, cmp, true, index, len, check_i);
    bool reverse =
        scev && nyr_cmp_proves_reverse_index_in_len(f, defs, scev, cfg, cmp,
                                                     true, index, len, check_i);
    if (ny_trace_enabled("NY_TRACE_BCE"))
      ny_trace_line("BCE",
                    "bce: guard-candidate check=%zu br=%zu target=L%lld cmp=%d "
                    "index=v%d len=v%d direct=%d reverse=%d",
                    check_i, i, (long long)br->imm, cmp ? (int)cmp->cmp : -1,
                    index, len, direct ? 1 : 0, reverse ? 1 : 0);
    safe = direct || reverse;
  }
  return safe;
}

static bool nyr_bce_trace_enabled(void) {
  return ny_dbg_is_enabled(NY_DBG_DEEP) || ny_trace_enabled("NY_TRACE_BCE");
}

static bool nyr_bounds_check_same_operands(const nyr_inst_t *a,
                                            const nyr_inst_t *b) {
  return a && b && a->op == NYIR_BOUNDS_CHECK && b->op == NYIR_BOUNDS_CHECK &&
         a->a == b->a && a->b == b->b && a->c == b->c && a->imm == b->imm;
}

/*
 * Remove a bounds check when an identical check dominates it.  SSA operands
 * cannot change between the two checks, and reaching the later check proves
 * the earlier one already succeeded, so this preserves both safety and trap
 * ordering while avoiding repeated checks across common CFG joins/blocks.
 */
static bool nyr_remove_dominated_bounds_checks(nyr_func_t *f) {
  if (!f || f->len == 0)
    return true;
  nyr_cfg_t cfg = {0};
  if (!nyr_cfg_build(f, &cfg))
    return false;
  for (size_t i = 0; i < f->len; ++i) {
    nyr_inst_t *check = &f->data[i];
    if (check->op != NYIR_BOUNDS_CHECK)
      continue;
    size_t use_block = cfg.inst_block[i];
    for (size_t j = 0; j < i; ++j) {
      const nyr_inst_t *prior = &f->data[j];
      if (!nyr_bounds_check_same_operands(prior, check))
        continue;
      size_t def_block = cfg.inst_block[j];
      if (!nyr_cfg_dominates(&cfg, def_block, use_block))
        continue;
      nyr_bce_trace(f, i, "eliminate",
                     "identical earlier bounds check dominates this check");
      nyr_inst_discard(check);
      break;
    }
  }
  nyr_cfg_free(&cfg);
  return true;
}

static void nyr_bce_trace(const nyr_func_t *f, size_t pc, const char *status,
                           const char *reason) {
  if (!nyr_bce_trace_enabled())
    return;
  const nyr_inst_t *in = f && pc < f->len ? &f->data[pc] : NULL;
  const char *file = in && in->debug.file ? in->debug.file : "<unknown>";
  if (ny_dbg_is_enabled(NY_DBG_DEEP) && !ny_trace_enabled("NY_TRACE_BCE")) {
    ny_dbg_heuristic_event(
        "BCE",
        "%s pc=%zu at %s:%u:%u block_heat=%" PRIu64 " loop_heat=%" PRIu64
        " reason=%s",
        status ? status : "remark", pc, file, in ? in->debug.line : 0,
        in ? in->debug.column : 0, ny_native_profile_block_hot(pc),
        ny_native_profile_loop_hot(pc), reason ? reason : "none");
    return;
  }
  fprintf(stderr,
          "nyir bce: %s pc=%zu at %s:%u:%u block_heat=%" PRIu64
          " loop_heat=%" PRIu64 " reason=%s\n",
          status ? status : "remark", pc, file, in ? in->debug.line : 0,
          in ? in->debug.column : 0, ny_native_profile_block_hot(pc),
          ny_native_profile_loop_hot(pc), reason ? reason : "none");
}

/*
 * Bounds-check elimination. The first case folds simple masked comparisons.
 * The second case removes lowered list/byte-buffer checks on the taken or
 * fall-through edge of a single-predecessor loop guard (`if i < len goto body`,
 * `if i <= len - 1 goto body`, `if i >= len goto exit`, or
 * `if i > len - 1 goto exit`) when the checked offset and dynamic byte length
 * share the same scale.
 */
bool nyr_bounds_check_elim(nyr_func_t *f) {
  if (!f || f->next_value <= 0)
    return true;
  bool *known = calloc((size_t)f->next_value, sizeof(bool));
  int64_t *value = calloc((size_t)f->next_value, sizeof(int64_t));
  int *defs = nyr_build_defs(f);
  nyr_value_fact_t *facts = calloc((size_t)f->next_value, sizeof(*facts));
  if (!known || !value || !defs || !facts) {
    free(known);
    free(value);
    free(defs);
    free(facts);
    return false;
  }
  nyr_bce_tbuf_roots_t roots = {.f = f,
                                 .defs = defs,
                                 .cfg = NULL,
                                 .roots = NULL,
                                 .state = NULL,
                                 .blocks_reaching_load = NULL,
                                 .reach_queue = NULL,
                                 .reach_capacity = 0,
                                 .reach_load_i = SIZE_MAX,
                                 .root_trace_remaining = 32};
  bool have_facts =
      nyr_analyze_values(f, facts, (size_t)f->next_value, NULL, 0);
  if (!nyr_remove_dominated_bounds_checks(f)) {
    free(known);
    free(value);
    free(defs);
    free(facts);
    return false;
  }
  nyr_scev_info_t scev = {0};
  bool have_scev = nyr_scev_analyze(f, &scev);
  /*
   * BCE only changes non-terminators below; reuse this CFG for every
   * dominating-guard and fresh-buffer proof in the function.
   */
  nyr_cfg_t cfg = {0};
  bool have_cfg = nyr_cfg_build(f, &cfg);
  roots.cfg = have_cfg ? &cfg : NULL;

  size_t current_label_i = SIZE_MAX;
  int current_label = -1;
  size_t normalized_index_elided = 0;
  for (size_t i = 0; i < f->len; ++i) {
    nyr_inst_t *in = &f->data[i];
    if (in->op == NYIR_LABEL) {
      current_label_i = i;
      current_label = (int)in->imm;
    }
    /*
     * Lowered list indexing normalizes a negative index with
     * `select(index < 0, index + len, index)`. If the source index is
     * globally nonnegative by the non-address-taken local-store proof, the
     * false arm is exact on every execution. Bounds checks still need their
     * own proof and are deliberately left untouched here.
     */
    if (in->op == NYIR_SELECT_I64 && in->a >= 0 && in->c >= 0 &&
        in->a < f->next_value) {
      int condition = nyr_value_skip_copies(f, defs, in->a);
      int condition_def =
          condition >= 0 && condition < f->next_value ? defs[condition] : -1;
      int64_t zero = -1;
      const nyr_inst_t *cmp =
          condition_def >= 0 && (size_t)condition_def < f->len
              ? &f->data[condition_def]
              : NULL;
      bool canonical = cmp && cmp->op == NYIR_CMP_I64 &&
                       cmp->cmp == NYIR_CMP_LT && cmp->a >= 0 &&
                       nyr_const_i64_value(f, defs, cmp->b, &zero) &&
                       zero == 0;
      bool same_index =
          canonical && nyr_guard_values_same_at(f, defs, in->c, cmp->a, i);
      bool nonnegative =
          same_index && nyr_guard_local_constant_nonnegative(f, defs, cmp->a);
      if (same_index && !nonnegative && have_scev) {
        nyr_scev_affine_t index_range = {0};
        nonnegative = have_cfg &&
                      nyr_scev_affine_at_cfg(f, &scev, defs, &cfg, cmp->a, i,
                                              &index_range) &&
                      index_range.range.has_min && index_range.range.min >= 0;
      }
      if (same_index && !nonnegative)
        nonnegative = nyr_guarded_index_nonnegative(f, defs, cmp->a, i);
      if (nonnegative) {
        int false_arm = in->c;
        nir_make_copy(in, false_arm);
        ++normalized_index_elided;
        nyr_bce_trace(f, i, "simplify",
                       "negative-index normalization is unreachable for a "
                       "nonnegative local");
      }
    }
    if (in->op == NYIR_BOUNDS_CHECK && in->c >= 0 &&
        current_label_i != SIZE_MAX && !roots.roots && !roots.state) {
      roots.roots = malloc((size_t)f->next_value * sizeof(*roots.roots));
      roots.state = calloc((size_t)f->next_value, sizeof(*roots.state));
      if (!roots.roots || !roots.state) {
        free(roots.roots);
        free(roots.state);
        roots.roots = NULL;
        roots.state = NULL;
      }
    }
    if (in->op == NYIR_BOUNDS_CHECK && current_label_i != SIZE_MAX &&
        nyr_bounds_check_guard_safe(f, defs, i, current_label_i, current_label,
                                     have_scev ? &scev : NULL,
                                     have_cfg ? &cfg : NULL, &roots)) {
      nyr_bce_trace(f, i, "eliminate",
                     "dominating loop guard proves index within length");
      *in = (nyr_inst_t){.op = NYIR_NOP,
                          .dst = -1,
                          .a = -1,
                          .b = -1,
                          .c = -1,
                          .d = -1,
                          .e = -1,
                          .f = -1};
      continue;
    }
    if (in->op == NYIR_BOUNDS_CHECK && have_scev && have_cfg &&
        nyr_scev_tbuf_bounds_check_safe_cfg(f, &scev, defs, &cfg, i, in)) {
      nyr_bce_trace(f, i, "eliminate",
                     "bounded affine tbuf offset is within tracked length");
      nyr_inst_discard(in);
      continue;
    }
    if (in->op == NYIR_BOUNDS_CHECK && have_facts && in->b >= 0 &&
        in->b < f->next_value) {
      const nyr_range_t *off = &facts[in->b].range;
      bool proven = off->has_min && off->has_max && off->min >= 0;
      if (proven && in->c >= 0 && in->c < f->next_value) {
        const nyr_range_t *len = &facts[in->c].range;
        proven = len->has_min && len->min > 0 && off->max < len->min;
      } else if (proven && in->c < 0) {
        proven = in->imm > 0 && off->max < in->imm;
      } else {
        proven = false;
      }
      if (!proven && defs && in->b >= 0 && in->b < f->next_value &&
          defs[in->b] >= 0 && (size_t)defs[in->b] < f->len) {
        const nyr_range_t *inst_off = &f->data[defs[in->b]].range;
        if (inst_off->has_min && inst_off->has_max && inst_off->min >= 0) {
          if (in->c >= 0 && in->c < f->next_value) {
            const nyr_range_t *inst_len = &facts[in->c].range;
            if (!inst_len->has_min && defs[in->c] >= 0 &&
                (size_t)defs[in->c] < f->len)
              inst_len = &f->data[defs[in->c]].range;
            proven = inst_len->has_min && inst_len->min > 0 &&
                     inst_off->max < inst_len->min;
          } else if (in->c < 0) {
            proven = in->imm > 0 && inst_off->max < in->imm;
          }
        }
      }
      if (proven) {
        nyr_bce_trace(f, i, "eliminate",
                       "retained range facts prove byte offset within length");
        nyr_inst_discard(in);
        continue;
      }
    }
    if (in->op == NYIR_CONST_I64 && in->dst >= 0 && in->dst < f->next_value) {
      known[in->dst] = true;
      value[in->dst] = in->imm;
    }
    if (in->op != NYIR_CMP_I64 || in->a < 0 || in->b < 0)
      continue;
    /*
     * Keep control comparisons executable until SCEV can prove loop-header
     * intervals without confusing an in-loop range with the loop's exit.
     */
    if (in->cmp != NYIR_CMP_LT && in->cmp != NYIR_CMP_LE)
      continue;
    if (!known[in->b])
      continue;
    int64_t lim = value[in->b];
    /*
     * Find if a is AND with const mask.
     */
    for (size_t j = i; j > 0; --j) {
      const nyr_inst_t *def = &f->data[j - 1];
      if (def->dst != in->a)
        continue;
      if (def->op == NYIR_AND_I64 && def->b >= 0 && known[def->b]) {
        int64_t mask = value[def->b];
        if (mask >= 0 && lim > 0 &&
            (in->cmp == NYIR_CMP_LT ? mask < lim : mask <= lim)) {
          nir_make_const(in, 1);
        }
      }
      break;
    }
  }
  if (nyr_bce_trace_enabled()) {
    if (normalized_index_elided)
      ny_trace_line("BCE",
                    "bce: simplified %zu negative-index normalization(s) from "
                    "local nonnegative proofs",
                    normalized_index_elided);
    for (size_t i = 0; i < f->len; ++i) {
      const nyr_inst_t *check = &f->data[i];
      if (check->op != NYIR_BOUNDS_CHECK)
        continue;
      const char *reason =
          "no recognized dominating guard discharges the check";
      if (have_facts && check->b >= 0 && check->b < f->next_value &&
          (!facts[check->b].range.has_min || !facts[check->b].range.has_max)) {
        reason = "checked offset has no retained range fact";
      } else if (have_facts && check->b >= 0 && check->b < f->next_value &&
                 facts[check->b].range.has_min &&
                 facts[check->b].range.min < 0) {
        reason = "checked offset may be negative";
      } else if (check->c >= 0 && have_facts && check->c < f->next_value &&
                 (!facts[check->c].range.has_min ||
                  !facts[check->c].range.has_max)) {
        reason = "dynamic length has no retained range fact";
      } else if (check->c >= 0 && have_facts && check->b >= 0 &&
                 check->b < f->next_value && check->c < f->next_value &&
                 facts[check->b].range.has_max &&
                 facts[check->c].range.has_min &&
                 facts[check->b].range.max >= facts[check->c].range.min) {
        reason = "offset maximum is not proven below the length minimum";
      } else if (check->c < 0 && check->imm <= 0) {
        reason = "check has no positive static or dynamic length proof";
      }
      nyr_bce_trace(f, i, "retain", reason);
    }
  }
  if (have_cfg)
    nyr_cfg_free(&cfg);
  nyr_scev_free(&scev);
  free(facts);
  free(defs);
  free(known);
  free(value);
  free(roots.roots);
  free(roots.state);
  free(roots.blocks_reaching_load);
  free(roots.reach_queue);
  return true;
}

/*
 * Local e-graph seed: reassociate pure (a+b)+c chains and commute consts to
 * the right on ADD/MUL for CSE friendliness. Bounded single forward pass.
 */
bool nyr_egraph_local(nyr_func_t *f) {
  if (!f || f->next_value <= 0)
    return true;
  int *defs = nyr_build_defs(f);
  if (!defs)
    return false;
  for (size_t i = 0; i < f->len; ++i) {
    nyr_inst_t *in = &f->data[i];
    if ((in->op != NYIR_ADD_I64 && in->op != NYIR_MUL_I64) || in->a < 0 ||
        in->b < 0)
      continue;
    int ad = in->a < f->next_value ? defs[in->a] : -1;
    int bd = in->b < f->next_value ? defs[in->b] : -1;
    bool a_const =
        ad >= 0 && (size_t)ad < i && f->data[ad].op == NYIR_CONST_I64;
    bool b_const =
        bd >= 0 && (size_t)bd < i && f->data[bd].op == NYIR_CONST_I64;
    /*
     * Canonicalize commutative scalar ops with constants on the right.
     */
    if (a_const && !b_const) {
      int t = in->a;
      in->a = in->b;
      in->b = t;
    }
  }
  free(defs);
  return true;
}

/*
 * apply_rules / ny_isle_apply_nir live in isle.c (shared NYIR + machine form).
 */

/*
 * Small pure-function inliner: replace CALL to a known callee with a copy of
 * its straight-line body when body is tiny and pure.
 */
static const nyr_inline_callee_t *nyr_inline_callees;
static size_t nyr_inline_callee_count;
static const nyr_inline_callee_t **nyr_inline_callee_index;
static size_t nyr_inline_callee_index_cap;
typedef struct {
  uint64_t closure_formals;
  size_t closure_formal_limit;
  size_t optimization_credit;
  bool direct_recursive;
  bool invalid_local;
  bool nested_closure_call;
  bool has_control_flow;
} nyr_inline_callee_summary_t;
static nyr_inline_callee_summary_t *nyr_inline_callee_summaries;

static const nyr_inline_callee_t *
nyr_find_inline_callee_entry(const char *name);
static bool nyr_func_has_control_flow(const nyr_func_t *f);

static const char *nyr_inline_normalize_name(const char *name) {
  return name && strncmp(name, "ny_fn_", 6) == 0 ? name + 6 : name;
}

static uint64_t nyr_inline_name_hash(const char *name) {
  uint64_t hash = UINT64_C(1469598103934665603);
  for (const unsigned char *p = (const unsigned char *)name; *p; ++p) {
    hash ^= *p;
    hash *= UINT64_C(1099511628211);
  }
  return hash;
}

void nyr_set_inline_callees(const nyr_inline_callee_t *callees,
                             size_t count) {
  free(nyr_inline_callee_index);
  free(nyr_inline_callee_summaries);
  nyr_inline_callee_index = NULL;
  nyr_inline_callee_summaries = NULL;
  nyr_inline_callee_index_cap = 0;
  nyr_inline_callees = callees;
  nyr_inline_callee_count = count;
  if (!callees || count == 0 || count > SIZE_MAX / 2u)
    return;

  size_t capacity = 16;
  size_t needed = count * 2u;
  while (capacity < needed) {
    if (capacity > SIZE_MAX / 2u)
      return;
    capacity *= 2u;
  }
  const nyr_inline_callee_t **index = calloc(capacity, sizeof(*index));
  if (!index)
    return; /* Preserve the original linear lookup on allocation failure. */

  for (size_t i = 0; i < count; ++i) {
    const char *name = nyr_inline_normalize_name(callees[i].name);
    if (!name)
      continue;
    size_t slot = (size_t)nyr_inline_name_hash(name) & (capacity - 1u);
    while (index[slot]) {
      const char *present = nyr_inline_normalize_name(index[slot]->name);
      if (present && strcmp(present, name) == 0)
        break; /* Match the linear search's first duplicate. */
      slot = (slot + 1u) & (capacity - 1u);
    }
    if (!index[slot])
      index[slot] = &callees[i];
  }
  nyr_inline_callee_index = index;
  nyr_inline_callee_index_cap = capacity;

  nyr_inline_callee_summary_t *summaries = calloc(count, sizeof(*summaries));
  if (!summaries)
    return;
  nyr_inline_callee_summaries = summaries;
  for (size_t i = 0; i < count; ++i) {
    const nyr_func_t *func = callees[i].func;
    if (!func)
      continue;
    const char *name = callees[i].name;
    summaries[i].has_control_flow = nyr_func_has_control_flow(func);
    for (size_t k = 0; k < func->len; ++k) {
      const nyr_inst_t *inst = &func->data[k];
      if (inst->op == NYIR_CALL && inst->symbol && name &&
          strcmp(inst->symbol, name) == 0)
        summaries[i].direct_recursive = true;
      if ((inst->op == NYIR_LOAD_LOCAL || inst->op == NYIR_STORE_LOCAL ||
           inst->op == NYIR_ADDR_LOCAL) &&
          (inst->imm < 0 || inst->imm >= INT_MAX))
        summaries[i].invalid_local = true;
      if (inst->op == NYIR_LOAD_LOCAL && inst->imm >= 0 &&
          inst->scalar_class == NYIR_SCALAR_CLOSURE) {
        const size_t limit = (size_t)inst->imm + 1u;
        if (summaries[i].closure_formal_limit == 0 ||
            limit < summaries[i].closure_formal_limit)
          summaries[i].closure_formal_limit = limit;
        if (inst->imm < NYIR_CALL_MAX_ARGS)
          summaries[i].closure_formals |= UINT64_C(1) << inst->imm;
      }
      if (inst->op == NYIR_ALLOCA && summaries[i].optimization_credit < 64)
        summaries[i].optimization_credit += 4;
      else if (inst->op == NYIR_BOUNDS_CHECK &&
               summaries[i].optimization_credit < 64)
        summaries[i].optimization_credit += 3;
    }
  }
  /* Resolve nested closure boundaries once, rather than rescanning every
   * target body for each call site of the enclosing helper. */
  for (size_t i = 0; i < count; ++i) {
    const nyr_func_t *func = callees[i].func;
    if (!func)
      continue;
    for (size_t k = 0; k < func->len; ++k) {
      const nyr_inst_t *inst = &func->data[k];
      if (inst->op != NYIR_CALL || !inst->symbol)
        continue;
      const nyr_inline_callee_t *target =
          nyr_find_inline_callee_entry(inst->symbol);
      if (!target || !target->func)
        continue;
      size_t ti = (size_t)(target - nyr_inline_callees);
      if (ti >= count)
        continue;
      if (summaries[ti].closure_formal_limit > 0 &&
          summaries[ti].closure_formal_limit <= target->func->param_count) {
        summaries[i].nested_closure_call = true;
        break;
      }
    }
  }
}

bool nyr_inline_callees_reference_symbol(const char *symbol,
                                          const nyr_func_t *exclude) {
  for (size_t i = 0; i < nyr_inline_callee_count; ++i) {
    const nyr_func_t *callee = nyr_inline_callees[i].func;
    if (!callee || callee == exclude)
      continue;
    for (size_t k = 0; k < callee->len; ++k) {
      const nyr_inst_t *in = &callee->data[k];
      if (in->op == NYIR_ADDR_SYMBOL && in->symbol &&
          strcmp(in->symbol, symbol) == 0)
        return true;
    }
  }
  return false;
}

typedef struct {
  const char *symbol;
  int64_t slot;
  nyr_scalar_class_t scalar_class;
} nyr_private_entry_global_t;

static int nyr_private_entry_def(const nyr_func_t *f, int value) {
  if (!f || value < 0 || value >= f->next_value)
    return -1;
  for (size_t i = f->len; i > 0; --i)
    if (f->data[i - 1].dst == value)
      return (int)(i - 1);
  return -1;
}

static bool nyr_private_entry_raw_int(const nyr_func_t *f, int value,
                                       unsigned depth) {
  if (!f || value < 0 || value >= f->next_value || depth > 12)
    return false;
  int di = nyr_private_entry_def(f, value);
  if (di < 0)
    return false;
  const nyr_inst_t *in = &f->data[di];
  if (in->scalar_class == NYIR_SCALAR_I64_RAW)
    return true;
  if (in->scalar_class != NYIR_SCALAR_UNKNOWN)
    return false;
  switch (in->op) {
  case NYIR_CONST_I64:
  case NYIR_ADD_I64:
  case NYIR_SUB_I64:
  case NYIR_MUL_I64:
  case NYIR_DIV_I64:
  case NYIR_MOD_I64:
  case NYIR_AND_I64:
  case NYIR_OR_I64:
  case NYIR_XOR_I64:
  case NYIR_SHL_I64:
  case NYIR_SAR_I64:
    return in->op == NYIR_CONST_I64 ||
           (nyr_private_entry_raw_int(f, in->a, depth + 1) &&
            nyr_private_entry_raw_int(f, in->b, depth + 1));
  case NYIR_COPY:
    return nyr_private_entry_raw_int(f, in->a, depth + 1);
  default:
    return false;
  }
}

static bool nyr_private_entry_f64(const nyr_func_t *f, int value,
                                   unsigned depth) {
  if (!f || value < 0 || value >= f->next_value || depth > 12)
    return false;
  int di = nyr_private_entry_def(f, value);
  if (di < 0)
    return false;
  const nyr_inst_t *in = &f->data[di];
  if (in->scalar_class == NYIR_SCALAR_F64)
    return true;
  if (in->scalar_class != NYIR_SCALAR_UNKNOWN)
    return false;
  switch (in->op) {
  case NYIR_CONST_F64:
    return true;
  case NYIR_ADD_F64:
  case NYIR_SUB_F64:
  case NYIR_MUL_F64:
  case NYIR_DIV_F64:
    return nyr_private_entry_f64(f, in->a, depth + 1) &&
           nyr_private_entry_f64(f, in->b, depth + 1);
  case NYIR_COPY:
    return nyr_private_entry_f64(f, in->a, depth + 1);
  default:
    return false;
  }
}

/*
 * A typed-buffer handle may move through a top-level mutable binding between
 * loop iterations. Keep this check separate from raw integers: only values
 * already annotated by the front end or copies of such values may qualify.
 */
static bool nyr_private_entry_typed_buffer(const nyr_func_t *f, int value,
                                            unsigned depth) {
  if (!f || value < 0 || value >= f->next_value || depth > 12)
    return false;
  int di = nyr_private_entry_def(f, value);
  if (di < 0)
    return false;
  const nyr_inst_t *in = &f->data[di];
  if (in->scalar_class == NYIR_SCALAR_TYPED_BUFFER)
    return true;
  if (in->scalar_class != NYIR_SCALAR_UNKNOWN || in->op != NYIR_COPY)
    return false;
  return nyr_private_entry_typed_buffer(f, in->a, depth + 1);
}

static bool nyr_private_entry_tbuf_source_symbol(const nyr_func_t *f,
                                                  int value,
                                                  const char **symbol,
                                                  unsigned depth) {
  if (!f || !symbol || value < 0 || value >= f->next_value || depth > 12)
    return false;
  int di = nyr_private_entry_def(f, value);
  if (di < 0)
    return false;
  const nyr_inst_t *in = &f->data[di];
  if (in->op == NYIR_COPY)
    return nyr_private_entry_tbuf_source_symbol(f, in->a, symbol, depth + 1);
  if (in->op != NYIR_LOAD_I64 || in->a < 0)
    return false;
  int ai = nyr_private_entry_def(f, in->a);
  if (ai < 0 || f->data[ai].op != NYIR_ADDR_SYMBOL || !f->data[ai].symbol)
    return false;
  *symbol = f->data[ai].symbol;
  return true;
}

/*
 * Prove that every use of a global slot is a canonical load/store and that
 * every value written is either an explicit typed-buffer handle or a load
 * from another slot proved by this same recursive walk. Cycles are allowed
 * only when the connected component has at least one explicit typed-buffer
 * producer.
 */
static bool nyr_private_entry_symbol_is_tbuf(const nyr_func_t *f,
                                              const nyr_use_def_t *uses,
                                              const char *symbol,
                                              const char **stack,
                                              unsigned depth, bool *has_seed) {
  if (!f || !uses || !symbol || !stack || !has_seed || depth > 12)
    return false;
  *has_seed = false;
  for (unsigned i = 0; i < depth; ++i)
    if (stack[i] && strcmp(stack[i], symbol) == 0)
      return true;
  stack[depth] = symbol;
  bool saw_load = false, saw_store = false;
  bool valid = true;
  for (size_t ai = 0; ai < f->len && valid; ++ai) {
    const nyr_inst_t *addr = &f->data[ai];
    if (addr->op != NYIR_ADDR_SYMBOL || !addr->symbol ||
        strcmp(addr->symbol, symbol) != 0 || addr->dst < 0 ||
        (size_t)addr->dst >= uses->value_count)
      continue;
    for (size_t ui = uses->offsets[addr->dst];
         ui < uses->offsets[addr->dst + 1] && valid; ++ui) {
      const nyr_inst_t *user = &f->data[uses->users[ui]];
      if (user->a != addr->dst || user->op == NYIR_NOP ||
          (user->flags & NYIR_INST_F_MEM_BYTE)) {
        valid = false;
      } else if (user->op == NYIR_LOAD_I64 &&
                 (user->scalar_class == NYIR_SCALAR_TYPED_BUFFER ||
                  user->scalar_class == NYIR_SCALAR_I64_TAGGED ||
                  user->scalar_class == NYIR_SCALAR_POINTER ||
                  user->scalar_class == NYIR_SCALAR_UNKNOWN)) {
        saw_load = true;
      } else if (user->op == NYIR_STORE_I64 && user->c >= 0) {
        saw_store = true;
        if (nyr_private_entry_typed_buffer(f, user->c, 0)) {
          *has_seed = true;
          continue;
        }
        const char *source = NULL;
        bool source_seed = false;
        if (!nyr_private_entry_tbuf_source_symbol(f, user->c, &source, 0) ||
            !nyr_private_entry_symbol_is_tbuf(f, uses, source, stack,
                                               depth + 1, &source_seed)) {
          valid = false;
        } else {
          *has_seed |= source_seed;
        }
      } else {
        valid = false;
      }
    }
  }
  return valid && saw_load && saw_store;
}

static bool nyr_private_entry_tbuf_initialized_before_loads(
    const nyr_func_t *f, const nyr_use_def_t *uses, const char *symbol) {
  nyr_cfg_t cfg = {0};
  if (!nyr_cfg_build(f, &cfg))
    return false;
  bool saw_load = false;
  bool safe = true;
  for (size_t ai = 0; ai < f->len && safe; ++ai) {
    const nyr_inst_t *addr = &f->data[ai];
    if (addr->op != NYIR_ADDR_SYMBOL || !addr->symbol ||
        strcmp(addr->symbol, symbol) != 0 || addr->dst < 0 ||
        (size_t)addr->dst >= uses->value_count)
      continue;
    for (size_t ui = uses->offsets[addr->dst];
         ui < uses->offsets[addr->dst + 1] && safe; ++ui) {
      size_t load_pc = uses->users[ui];
      if (f->data[load_pc].op != NYIR_LOAD_I64)
        continue;
      saw_load = true;
      size_t load_block = cfg.inst_block[load_pc];
      bool initialized = false;
      for (size_t si = 0; si < f->len && !initialized; ++si) {
        const nyr_inst_t *store = &f->data[si];
        if (store->op != NYIR_STORE_I64 || store->c < 0 ||
            !nyr_private_entry_typed_buffer(f, store->c, 0) || store->a < 0 ||
            store->a >= f->next_value)
          continue;
        int store_addr_def = nyr_private_entry_def(f, store->a);
        if (store_addr_def < 0 ||
            f->data[store_addr_def].op != NYIR_ADDR_SYMBOL ||
            !f->data[store_addr_def].symbol ||
            strcmp(f->data[store_addr_def].symbol, symbol) != 0)
          continue;
        size_t store_block = cfg.inst_block[si];
        initialized = store_block == load_block
                          ? si < load_pc
                          : nyr_cfg_dominates(&cfg, store_block, load_block);
      }
      if (!initialized)
        safe = false;
    }
  }
  nyr_cfg_free(&cfg);
  return safe && saw_load;
}

static int64_t
nyr_private_entry_slot_for(const nyr_private_entry_global_t *globals,
                            size_t count, const char *symbol) {
  for (size_t i = 0; i < count; ++i)
    if (globals[i].symbol && symbol && strcmp(globals[i].symbol, symbol) == 0)
      return globals[i].slot;
  return -1;
}

/*
 * Top-level mutable script bindings are represented as globals so functions
 * can capture them. In the synthesized entry body, raw-int, f64, and
 * typed-buffer bindings that no user function references can be safely kept
 * in local slots instead. This removes global load/store pairs from loops and
 * lets mem2reg build ordinary induction and buffer PHIs.
 * The pass is called only for rt_main, never for reusable module functions.
 */
bool nyr_promote_private_entry_globals(nyr_func_t *f) {
  if (!f || f->len == 0 || f->next_value <= 0)
    return true;
  nyr_use_def_t uses = {0};
  if (!nyr_build_use_def(f, &uses))
    return false;
  size_t cap = 0;
  for (size_t i = 0; i < f->len; ++i)
    cap += f->data[i].op == NYIR_ADDR_SYMBOL && f->data[i].symbol;
  nyr_private_entry_global_t *globals =
      cap ? calloc(cap, sizeof(*globals)) : NULL;
  const char **considered = cap ? calloc(cap, sizeof(*considered)) : NULL;
  if (cap && (!globals || !considered)) {
    free(globals);
    free(considered);
    nyr_use_def_free(&uses);
    return false;
  }
  size_t count = 0, considered_count = 0;
  int64_t next_slot = 0;
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if ((in->op == NYIR_LOAD_LOCAL || in->op == NYIR_STORE_LOCAL ||
         in->op == NYIR_ADDR_LOCAL) &&
        in->imm >= next_slot)
      next_slot = in->imm + 1;
  }
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *addr = &f->data[i];
    if (addr->op != NYIR_ADDR_SYMBOL || !addr->symbol || addr->dst < 0 ||
        (size_t)addr->dst >= uses.value_count)
      continue;
    bool already_considered = false;
    for (size_t ci = 0; ci < considered_count; ++ci)
      if (strcmp(considered[ci], addr->symbol) == 0) {
        already_considered = true;
        break;
      }
    if (already_considered)
      continue;
    considered[considered_count++] = addr->symbol;
    bool safe = true, saw_load = false, saw_store = false;
    nyr_scalar_class_t binding_class = NYIR_SCALAR_UNKNOWN;
    const char *tbuf_stack[13] = {0};
    bool tbuf_seed = false;
    if (nyr_private_entry_symbol_is_tbuf(f, &uses, addr->symbol, tbuf_stack, 0,
                                          &tbuf_seed) &&
        tbuf_seed &&
        nyr_private_entry_tbuf_initialized_before_loads(f, &uses,
                                                         addr->symbol))
      binding_class = NYIR_SCALAR_TYPED_BUFFER;
    const char *reject_reason = "none";
    for (size_t ai = 0; ai < f->len && safe; ++ai) {
      const nyr_inst_t *symbol_addr = &f->data[ai];
      if (symbol_addr->op != NYIR_ADDR_SYMBOL || !symbol_addr->symbol ||
          strcmp(symbol_addr->symbol, addr->symbol) != 0 ||
          symbol_addr->dst < 0 || (size_t)symbol_addr->dst >= uses.value_count)
        continue;
      size_t begin = uses.offsets[symbol_addr->dst];
      size_t end = uses.offsets[symbol_addr->dst + 1];
      for (size_t ui = begin; ui < end && safe; ++ui) {
        size_t user_idx = uses.users[ui];
        const nyr_inst_t *user = &f->data[user_idx];
        if (user->a != symbol_addr->dst || user->op == NYIR_NOP ||
            (user->flags & NYIR_INST_F_MEM_BYTE)) {
          safe = false;
          reject_reason = "address-escapes-or-noncanonical-use";
          break;
        }
        if (user->op == NYIR_LOAD_I64 &&
            (user->scalar_class == NYIR_SCALAR_I64_RAW ||
             user->scalar_class == NYIR_SCALAR_F64 ||
             user->scalar_class == NYIR_SCALAR_TYPED_BUFFER ||
             (binding_class == NYIR_SCALAR_TYPED_BUFFER &&
              (user->scalar_class == NYIR_SCALAR_I64_TAGGED ||
               user->scalar_class == NYIR_SCALAR_POINTER)))) {
          nyr_scalar_class_t load_class =
              binding_class == NYIR_SCALAR_TYPED_BUFFER
                  ? NYIR_SCALAR_TYPED_BUFFER
                  : user->scalar_class;
          if (binding_class != NYIR_SCALAR_UNKNOWN &&
              binding_class != load_class) {
            safe = false;
            reject_reason = "inconsistent-load-class";
            break;
          }
          binding_class = load_class;
          saw_load = true;
        } else if (user->op == NYIR_LOAD_I64) {
          safe = false;
          reject_reason = "load-is-not-raw-int";
        } else if (user->op == NYIR_STORE_I64 && user->c >= 0) {
          nyr_scalar_class_t stored_class = NYIR_SCALAR_UNKNOWN;
          if (nyr_private_entry_typed_buffer(f, user->c, 0)) {
            stored_class = NYIR_SCALAR_TYPED_BUFFER;
          } else if (nyr_private_entry_f64(f, user->c, 0)) {
            stored_class = NYIR_SCALAR_F64;
          } else if (binding_class == NYIR_SCALAR_TYPED_BUFFER) {
            const char *source = NULL;
            const char *source_stack[13] = {0};
            bool source_seed = false;
            if (nyr_private_entry_tbuf_source_symbol(f, user->c, &source, 0) &&
                nyr_private_entry_symbol_is_tbuf(
                    f, &uses, source, source_stack, 0, &source_seed) &&
                source_seed)
              stored_class = NYIR_SCALAR_TYPED_BUFFER;
          } else if (nyr_private_entry_raw_int(f, user->c, 0)) {
            stored_class = NYIR_SCALAR_I64_RAW;
          }
          if (stored_class == NYIR_SCALAR_UNKNOWN ||
              (binding_class != NYIR_SCALAR_UNKNOWN &&
               binding_class != stored_class)) {
            safe = false;
            reject_reason = "stored-value-class-mismatch";
            break;
          }
          binding_class = stored_class;
          saw_store = true;
        } else {
          safe = false;
          reject_reason = user->op == NYIR_STORE_I64
                              ? "stored-value-is-not-raw-int"
                              : "unsupported-address-use";
        }
      }
    }
    /*
     * Floating top-level bindings still cross helper-call and loop boundaries
     * through their symbol homes.  Localizing them here loses that canonical
     * carrier in large UI entry functions, where later calls can observe a
     * stale or integer-class local and corrupt the float state.  Keep f64
     * globals in their proven representation until the entry-local pass has
     * an interprocedural preservation proof.
     */
    if (binding_class == NYIR_SCALAR_F64) {
      safe = false;
      reject_reason = "f64-entry-localization-unproven";
    }
    if (!safe || !saw_load || !saw_store || next_slot >= INT64_MAX) {
      if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
        ny_dbg_heuristic_event("ENTRY_LOCALIZE",
                               "keep global=%s reason=%s load=%d store=%d",
                               addr->symbol,
                               !safe        ? reject_reason
                               : !saw_load  ? "no-raw-int-load"
                               : !saw_store ? "no-raw-int-store"
                                            : "local-slot-overflow",
                               (int)saw_load, (int)saw_store);
      continue;
    }
    /*
     * A module/user function that names this global keeps its storage.
     */
    if (nyr_inline_callees_reference_symbol(addr->symbol, f)) {
      safe = false;
      reject_reason = "referenced-by-user-function";
    }
    if (!safe) {
      if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
        ny_dbg_heuristic_event(
            "ENTRY_LOCALIZE", "keep global=%s reason=%s load=%d store=%d",
            addr->symbol, reject_reason, (int)saw_load, (int)saw_store);
      continue;
    }
    globals[count++] =
        (nyr_private_entry_global_t){addr->symbol, next_slot++, binding_class};
  }
  free(considered);
  considered = NULL;
  if (count) {
    size_t init_count = count * 2;
    if (init_count > SIZE_MAX - f->len || f->next_value > INT_MAX - (int)count)
      goto fail;
    size_t new_len = f->len + init_count;
    size_t new_cap = f->cap;
    if (new_len > new_cap) {
      new_cap = new_cap ? new_cap : 64;
      while (new_cap < new_len) {
        if (new_cap > SIZE_MAX / 2)
          goto fail;
        new_cap *= 2;
      }
      nyr_inst_t *new_data =
          ny_realloc_array(f->data, new_cap, sizeof(*new_data));
      if (!new_data)
        goto fail;
      f->data = new_data;
      f->cap = new_cap;
    }
    memmove(f->data + init_count, f->data, f->len * sizeof(*f->data));
    for (size_t i = 0; i < count; ++i) {
      int zero = f->next_value++;
      f->data[i * 2] = (nyr_inst_t){
          .op = globals[i].scalar_class == NYIR_SCALAR_F64 ? NYIR_CONST_F64
                                                           : NYIR_CONST_I64,
          .dst = zero,
          .a = -1,
          .b = -1,
          .imm = 0,
          .scalar_class = globals[i].scalar_class == NYIR_SCALAR_F64
                              ? NYIR_SCALAR_F64
                              : NYIR_SCALAR_I64_RAW};
      f->data[i * 2 + 1] =
          (nyr_inst_t){.op = NYIR_STORE_LOCAL,
                        .dst = -1,
                        .a = zero,
                        .b = -1,
                        .c = -1,
                        .d = -1,
                        .e = -1,
                        .f = -1,
                        .imm = globals[i].slot,
                        .scalar_class = globals[i].scalar_class,
                        .effects = NYIR_EFFECT_WRITE_LOCAL};
    }
    f->len = new_len;
    for (size_t i = init_count; i < f->len; ++i) {
      nyr_inst_t *in = &f->data[i];
      if (in->op == NYIR_LOAD_I64 && in->a >= 0) {
        int di = nyr_private_entry_def(f, in->a);
        const char *symbol = di >= 0 ? f->data[di].symbol : NULL;
        int64_t slot = nyr_private_entry_slot_for(globals, count, symbol);
        if (di >= 0 && f->data[di].op == NYIR_ADDR_SYMBOL && slot >= 0) {
          size_t gi = (size_t)(slot - (next_slot - (int64_t)count));
          in->op = NYIR_LOAD_LOCAL;
          in->imm = slot;
          in->a = in->b = in->c = in->d = in->e = in->f = -1;
          in->effects = NYIR_EFFECT_READ_LOCAL;
          /*
           * The old class described the global address, not this local read.
           */
          in->alias_class = 0;
          in->scalar_class = globals[gi].scalar_class;
          nyr_clear_derived_facts(in);
        }
      } else if (in->op == NYIR_STORE_I64 && in->a >= 0 && in->c >= 0) {
        int di = nyr_private_entry_def(f, in->a);
        const char *symbol = di >= 0 ? f->data[di].symbol : NULL;
        int64_t slot = nyr_private_entry_slot_for(globals, count, symbol);
        if (di >= 0 && f->data[di].op == NYIR_ADDR_SYMBOL && slot >= 0) {
          in->op = NYIR_STORE_LOCAL;
          in->a = in->c;
          in->b = in->c = in->d = in->e = in->f = -1;
          in->imm = slot;
          in->effects = NYIR_EFFECT_WRITE_LOCAL;
          in->scalar_class =
              globals[(size_t)(slot - (next_slot - (int64_t)count))]
                  .scalar_class;
          /*
           * The old class described the global address, not this local write.
           */
          in->alias_class = 0;
        }
      }
    }
    for (size_t i = init_count; i < f->len; ++i) {
      nyr_inst_t *in = &f->data[i];
      if (in->op == NYIR_ADDR_SYMBOL && in->symbol &&
          nyr_private_entry_slot_for(globals, count, in->symbol) >= 0) {
        nyr_debug_loc_t debug = in->debug;
        *in = (nyr_inst_t){.op = NYIR_NOP,
                            .dst = -1,
                            .a = -1,
                            .b = -1,
                            .c = -1,
                            .d = -1,
                            .e = -1,
                            .f = -1,
                            .debug = debug};
      }
    }
    if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
      ny_dbg_heuristic_event("ENTRY_LOCALIZE",
                             "promoted %zu private scalar/typed-buffer "
                             "globals to entry locals; next_slot=%" PRId64,
                             count, next_slot);
  }
  free(globals);
  nyr_use_def_free(&uses);
  return true;
fail:
  free(considered);
  free(globals);
  nyr_use_def_free(&uses);
  return false;
}

static const nyr_inline_callee_t *
nyr_find_inline_callee_entry(const char *name) {
  if (!name)
    return NULL;
  name = nyr_inline_normalize_name(name);
  if (nyr_inline_callee_index && nyr_inline_callee_index_cap) {
    size_t slot = (size_t)nyr_inline_name_hash(name) &
                  (nyr_inline_callee_index_cap - 1u);
    for (size_t probes = 0; probes < nyr_inline_callee_index_cap; ++probes) {
      const nyr_inline_callee_t *entry = nyr_inline_callee_index[slot];
      if (!entry)
        return NULL;
      const char *candidate = nyr_inline_normalize_name(entry->name);
      if (candidate && strcmp(candidate, name) == 0)
        return entry;
      slot = (slot + 1u) & (nyr_inline_callee_index_cap - 1u);
    }
    return NULL;
  }
  for (size_t i = 0; i < nyr_inline_callee_count; ++i) {
    const char *cname = nyr_inline_normalize_name(nyr_inline_callees[i].name);
    if (!cname)
      continue;
    /*
     * Lowering already resolved declaration identity. A leaf match can
     * substitute an unrelated imported function, including its ABI/body.
     */
    if (strcmp(cname, name) == 0)
      return &nyr_inline_callees[i];
  }
  return NULL;
}

static const nyr_func_t *nyr_find_inline_callee(const char *name) {
  const nyr_inline_callee_t *entry = nyr_find_inline_callee_entry(name);
  return entry ? entry->func : NULL;
}

static const nyr_inline_callee_summary_t *
nyr_find_inline_callee_summary(const char *name) {
  const nyr_inline_callee_t *entry = nyr_find_inline_callee_entry(name);
  if (!entry || !nyr_inline_callee_summaries)
    return NULL;
  const size_t index = (size_t)(entry - nyr_inline_callees);
  return index < nyr_inline_callee_count ? &nyr_inline_callee_summaries[index]
                                          : NULL;
}

static bool nyr_func_is_inline_candidate(const nyr_func_t *f) {
  if (!f || f->len == 0 || f->len > 48)
    return false;
  if (f->len > 24 && !ny_native_profile_should_inline(f->len))
    return false;
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->op == NYIR_CALL || in->op == NYIR_PHI || in->op == NYIR_BR ||
        in->op == NYIR_BR_IF || (in->op == NYIR_LABEL && i > 0))
      return false;
    if (in->op == NYIR_STORE_LOCAL &&
        (in->imm < 0 || in->imm >= (int64_t)f->param_count))
      return false;
  }
  return true;
}

static bool nyr_func_has_control_flow(const nyr_func_t *f) {
  if (!f)
    return false;
  for (size_t i = 0; i < f->len; ++i) {
    nyr_op_t op = f->data[i].op;
    if (op == NYIR_BR || op == NYIR_BR_IF || op == NYIR_PHI ||
        (op == NYIR_LABEL && i > 0))
      return true;
  }
  return false;
}

static bool nyr_is_i64_binop(nyr_op_t op) {
  return op == NYIR_ADD_I64 || op == NYIR_MUL_I64 || op == NYIR_SUB_I64 ||
         op == NYIR_AND_I64 || op == NYIR_OR_I64 || op == NYIR_XOR_I64 ||
         op == NYIR_SHL_I64 || op == NYIR_SAR_I64 || op == NYIR_DIV_I64 ||
         op == NYIR_MOD_I64;
}

/*
 * Ensure room for `extra` more instructions at index `at` (shifts tail).
 */
static bool nyr_ensure_insert(nyr_func_t *f, size_t at, size_t extra) {
  if (!f || extra == 0)
    return true;
  if (f->len + extra > f->cap) {
    size_t nc = f->cap ? f->cap * 2 : 64;
    while (nc < f->len + extra)
      nc *= 2;
    nyr_inst_t *nd = ny_realloc_array(f->data, nc, sizeof(*nd));
    if (!nd)
      return false;
    f->data = nd;
    f->cap = nc;
  }
  memmove(&f->data[at + extra], &f->data[at],
          (f->len - at) * sizeof(nyr_inst_t));
  f->len += extra;
  return true;
}

typedef struct {
  bool known;
  int64_t value;
} nyr_ipa_const_fact_t;

typedef struct {
  int64_t address;
  int64_t value;
} nyr_ipa_memory_write_t;

typedef struct {
  bool complete;
  size_t count;
  nyr_ipa_memory_write_t writes[NYIR_CALL_MAX_ARGS];
} nyr_ipa_memory_writes_t;

static bool nyr_ipa_i64_binop(nyr_op_t op) {
  return op == NYIR_ADD_I64 || op == NYIR_SUB_I64 || op == NYIR_MUL_I64 ||
         op == NYIR_AND_I64 || op == NYIR_OR_I64 || op == NYIR_XOR_I64 ||
         op == NYIR_SHL_I64 || op == NYIR_SAR_I64 || op == NYIR_ROR32_I64;
}

static bool nyr_ipa_i64_unop(nyr_op_t op) {
  return op == NYIR_BSWAP_I64 || op == NYIR_BSWAP32_I64;
}

static bool nyr_ipa_const_eval(nyr_op_t op, int64_t a, int64_t b,
                                nyr_cmp_t cmp, int64_t *out) {
  uint64_t ua = (uint64_t)a;
  uint64_t ub = (uint64_t)b;
  if (!out)
    return false;
  switch (op) {
  case NYIR_ADD_I64:
    *out = (int64_t)(ua + ub);
    return true;
  case NYIR_SUB_I64:
    *out = (int64_t)(ua - ub);
    return true;
  case NYIR_MUL_I64:
    *out = (int64_t)(ua * ub);
    return true;
  case NYIR_AND_I64:
    *out = (int64_t)(ua & ub);
    return true;
  case NYIR_OR_I64:
    *out = (int64_t)(ua | ub);
    return true;
  case NYIR_XOR_I64:
    *out = (int64_t)(ua ^ ub);
    return true;
  case NYIR_SHL_I64:
    *out = (int64_t)(ua << (ub & 63u));
    return true;
  case NYIR_SAR_I64:
    *out = (int64_t)(ua >> (ub & 63u));
    if (a < 0 && (ub & 63u) != 0)
      *out |= (int64_t)(UINT64_MAX << (64u - (ub & 63u)));
    return true;
  case NYIR_ROR32_I64: {
    uint32_t x = (uint32_t)ua;
    unsigned shift = (unsigned)(ub & 31u);
    uint32_t r = shift == 0 ? x : (x >> shift) | (x << (32u - shift));
    *out = (int64_t)r;
    return true;
  }
  case NYIR_CMP_I64: {
    bool result = false;
    switch (cmp) {
    case NYIR_CMP_EQ:
      result = a == b;
      break;
    case NYIR_CMP_NE:
      result = a != b;
      break;
    case NYIR_CMP_LT:
      result = a < b;
      break;
    case NYIR_CMP_LE:
      result = a <= b;
      break;
    case NYIR_CMP_GT:
      result = a > b;
      break;
    case NYIR_CMP_GE:
      result = a >= b;
      break;
    default:
      return false;
    }
    *out = result ? 1 : 0;
    return true;
  }
  default:
    return false;
  }
}

static bool nyr_ipa_body_is_scalar_pure(const nyr_func_t *callee) {
  if (!callee || callee->len == 0 || callee->param_count == 0)
    return callee && callee->len != 0;
  for (size_t i = 0; i < callee->len; ++i) {
    const nyr_inst_t *in = &callee->data[i];
    if (in->op == NYIR_CALL) {
      /*
       * Recursive evaluation is allowed only for a direct self-call. The
       * caller's body is checked in full here, so recursion cannot introduce
       * effects that are absent from this same body.
       */
      if (!in->symbol || nyr_find_inline_callee(in->symbol) != callee)
        return false;
      continue;
    }
    unsigned effects = in->effects | nyr_inst_effects(in);
    const unsigned forbidden =
        NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_WRITE_MEMORY |
        NYIR_EFFECT_VOLATILE | NYIR_EFFECT_MAY_TRAP | NYIR_EFFECT_ALLOCATION |
        NYIR_EFFECT_UNKNOWN_SIDE_EFFECT | NYIR_EFFECT_IO | NYIR_EFFECT_THREAD |
        NYIR_EFFECT_FFI | NYIR_EFFECT_FENV;
    if ((effects & forbidden) != 0)
      return false;
    switch (in->op) {
    case NYIR_NOP:
    case NYIR_LABEL:
    case NYIR_CONST_I64:
    case NYIR_COPY:
    case NYIR_LOAD_LOCAL:
    case NYIR_STORE_LOCAL:
    case NYIR_RET:
    case NYIR_PHI:
    case NYIR_BR:
    case NYIR_BR_IF:
    case NYIR_ADD_I64:
    case NYIR_SUB_I64:
    case NYIR_MUL_I64:
    case NYIR_AND_I64:
    case NYIR_OR_I64:
    case NYIR_XOR_I64:
    case NYIR_SHL_I64:
    case NYIR_SAR_I64:
    case NYIR_ROR32_I64:
    case NYIR_BSWAP_I64:
    case NYIR_BSWAP32_I64:
    case NYIR_CMP_I64:
    case NYIR_SELECT_I64:
      break;
    default:
      /*
       * Reject all operations outside the scalar interpreter.
       */
      return false;
    }
  }
  return true;
}

static bool nyr_ipa_store_is_formal_pointer(const nyr_func_t *callee,
                                             const nyr_inst_t *store,
                                             int *formal_slot_out) {
  if (!callee || !store || store->op != NYIR_STORE_I64 || store->a < 0 ||
      store->b >= 0 || store->c < 0)
    return false;
  int formal_slot = -1;
  for (size_t i = 0; i < callee->len; ++i) {
    const nyr_inst_t *in = &callee->data[i];
    if (in->op == NYIR_LOAD_LOCAL && in->dst == store->a && in->imm >= 0 &&
        (uint64_t)in->imm < callee->param_count) {
      formal_slot = (int)in->imm;
      break;
    }
  }
  if (formal_slot < 0)
    return false;
  for (size_t i = 0; i < callee->len; ++i) {
    const nyr_inst_t *in = &callee->data[i];
    if (in->op == NYIR_STORE_LOCAL && in->imm == formal_slot)
      return false;
  }
  if (formal_slot_out)
    *formal_slot_out = formal_slot;
  return true;
}

static bool
nyr_ipa_body_is_scalar_with_formal_stores(const nyr_func_t *callee) {
  if (!callee || !callee->len || !callee->param_count || callee->len > 128)
    return false;
  bool saw_formal_store = false;
  const unsigned forbidden =
      NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_WRITE_MEMORY |
      NYIR_EFFECT_VOLATILE | NYIR_EFFECT_MAY_TRAP | NYIR_EFFECT_ALLOCATION |
      NYIR_EFFECT_UNKNOWN_SIDE_EFFECT | NYIR_EFFECT_IO | NYIR_EFFECT_THREAD |
      NYIR_EFFECT_FFI | NYIR_EFFECT_FENV;
  for (size_t i = 0; i < callee->len; ++i) {
    const nyr_inst_t *in = &callee->data[i];
    if (in->op == NYIR_STORE_I64) {
      unsigned effects = in->effects | nyr_inst_effects(in);
      if (!nyr_ipa_store_is_formal_pointer(callee, in, NULL) ||
          (effects & NYIR_EFFECT_WRITE_MEMORY) == 0 ||
          (effects & ~(NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP)) != 0)
        return false;
      saw_formal_store = true;
      continue;
    }
    if (in->op == NYIR_CALL ||
        ((in->effects | nyr_inst_effects(in)) & forbidden) != 0)
      return false;
    switch (in->op) {
    case NYIR_NOP:
    case NYIR_LABEL:
    case NYIR_CONST_I64:
    case NYIR_COPY:
    case NYIR_LOAD_LOCAL:
    case NYIR_STORE_LOCAL:
    case NYIR_RET:
    case NYIR_PHI:
    case NYIR_BR:
    case NYIR_BR_IF:
    case NYIR_ADD_I64:
    case NYIR_SUB_I64:
    case NYIR_MUL_I64:
    case NYIR_AND_I64:
    case NYIR_OR_I64:
    case NYIR_XOR_I64:
    case NYIR_SHL_I64:
    case NYIR_SAR_I64:
    case NYIR_ROR32_I64:
    case NYIR_BSWAP_I64:
    case NYIR_BSWAP32_I64:
    case NYIR_CMP_I64:
    case NYIR_SELECT_I64:
      break;
    default:
      return false;
    }
  }
  return saw_formal_store;
}

static bool
nyr_ipa_effectful_result_summary_matches(const nyr_inst_t *call,
                                          const nyr_func_t *callee) {
  if (!call || !callee || (call->flags & NYIR_INST_F_EFFECTS_KNOWN) == 0 ||
      (call->effects & NYIR_EFFECT_CALL) == 0)
    return false;
  const unsigned observable =
      NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_WRITE_MEMORY |
      NYIR_EFFECT_VOLATILE | NYIR_EFFECT_MAY_TRAP | NYIR_EFFECT_ALLOCATION |
      NYIR_EFFECT_UNKNOWN_SIDE_EFFECT | NYIR_EFFECT_IO | NYIR_EFFECT_THREAD |
      NYIR_EFFECT_FFI | NYIR_EFFECT_FENV;
  unsigned body_effects = 0;
  for (size_t i = 0; i < callee->len; ++i)
    body_effects |= nyr_effective_effects(&callee->data[i]) & observable;
  return (call->effects & observable) == body_effects &&
         (body_effects & NYIR_EFFECT_WRITE_MEMORY) != 0;
}

static bool nyr_ipa_eval_callee(const nyr_func_t *callee, const int64_t *args,
                                 size_t arg_count, int64_t *result,
                                 size_t *instruction_budget,
                                 nyr_ipa_memory_writes_t *memory_writes,
                                 unsigned recursion_depth) {
  if (!callee || !args || !result || arg_count != callee->param_count ||
      !instruction_budget || recursion_depth > 32 ||
      (recursion_depth == 0 && !nyr_ipa_body_is_scalar_pure(callee) &&
       !nyr_ipa_body_is_scalar_with_formal_stores(callee)))
    return false;
  if (memory_writes)
    *memory_writes = (nyr_ipa_memory_writes_t){.complete = true};
  size_t value_count = callee->next_value > 0 ? (size_t)callee->next_value : 1;
  size_t local_count = callee->param_count;
  for (size_t i = 0; i < callee->len; ++i) {
    const nyr_inst_t *in = &callee->data[i];
    if ((in->op == NYIR_LOAD_LOCAL || in->op == NYIR_STORE_LOCAL) &&
        in->imm >= 0 && (size_t)in->imm + 1 > local_count)
      local_count = (size_t)in->imm + 1;
  }
  nyr_ipa_const_fact_t *facts = calloc(value_count, sizeof(*facts));
  nyr_ipa_const_fact_t *locals =
      calloc(local_count ? local_count : 1, sizeof(*locals));
  if (!facts || !locals) {
    free(facts);
    free(locals);
    return false;
  }
  for (size_t i = 0; i < arg_count; ++i) {
    locals[i].known = true;
    locals[i].value = args[i];
  }
  bool found_return = false;
  int64_t return_value = 0;
  bool ok = true;
  int64_t current_label = -1;
  int64_t predecessor_label = -1;
  bool has_predecessor = false;
  size_t pc = 0;
  while (ok && pc < callee->len && !found_return) {
    if (*instruction_budget == 0) {
      ok = false;
      break;
    }
    --*instruction_budget;
    size_t i = pc++;
    const nyr_inst_t *in = &callee->data[i];
    int64_t value = 0;
    switch (in->op) {
    case NYIR_NOP:
      break;
    case NYIR_LABEL:
      current_label = in->imm;
      break;
    case NYIR_CONST_I64:
      if (in->dst < 0 || (size_t)in->dst >= value_count) {
        ok = false;
        break;
      }
      facts[in->dst] = (nyr_ipa_const_fact_t){true, in->imm};
      break;
    case NYIR_COPY:
      if (in->dst < 0 || in->a < 0 || (size_t)in->dst >= value_count ||
          (size_t)in->a >= value_count || !facts[in->a].known) {
        ok = false;
        break;
      }
      facts[in->dst] = facts[in->a];
      break;
    case NYIR_LOAD_LOCAL:
      if (in->dst < 0 || in->imm < 0 || (size_t)in->dst >= value_count ||
          (size_t)in->imm >= local_count || !locals[in->imm].known) {
        ok = false;
        break;
      }
      facts[in->dst] = locals[in->imm];
      break;
    case NYIR_STORE_LOCAL:
      if (in->imm < 0 || (size_t)in->imm >= local_count || in->a < 0 ||
          (size_t)in->a >= value_count || !facts[in->a].known) {
        ok = false;
        break;
      }
      locals[in->imm] = facts[in->a];
      break;
    case NYIR_STORE_I64:
      /*
       * This store's effect is preserved by the caller's retained call.
       * Record the actual address/value pair so an immediately following
       * caller load can use the last write without removing the call barrier.
       */
      {
        int formal_slot = -1;
        if (!nyr_ipa_store_is_formal_pointer(callee, in, &formal_slot) ||
            in->a < 0 || in->c < 0 || (size_t)in->a >= value_count ||
            (size_t)in->c >= value_count || !facts[in->a].known ||
            !facts[in->c].known || formal_slot < 0 ||
            (size_t)formal_slot >= arg_count) {
          ok = false;
          break;
        }
        if (memory_writes) {
          int64_t address = facts[in->a].value;
          int64_t stored_value = facts[in->c].value;
          size_t slot = 0;
          while (slot < memory_writes->count &&
                 memory_writes->writes[slot].address != address)
            ++slot;
          if (slot < memory_writes->count) {
            memory_writes->writes[slot].value = stored_value;
          } else if (memory_writes->count <
                     sizeof(memory_writes->writes) /
                         sizeof(memory_writes->writes[0])) {
            memory_writes->writes[memory_writes->count++] =
                (nyr_ipa_memory_write_t){address, stored_value};
          } else {
            memory_writes->complete = false;
          }
        }
      }
      break;
    case NYIR_PHI: {
      if (in->dst < 0 || (size_t)in->dst >= value_count || !has_predecessor) {
        ok = false;
        break;
      }
      bool matched = false;
      for (size_t k = 0; k < in->phi_incoming_len; ++k) {
        const nyr_phi_incoming_t *edge = &in->phi_incoming[k];
        if (edge->predecessor_label != predecessor_label)
          continue;
        if (edge->value < 0 || (size_t)edge->value >= value_count ||
            !facts[edge->value].known) {
          ok = false;
          break;
        }
        facts[in->dst] = facts[edge->value];
        matched = true;
        break;
      }
      if (!matched)
        ok = false;
      break;
    }
    case NYIR_SELECT_I64:
      if (in->dst < 0 || in->a < 0 || in->b < 0 || in->c < 0 ||
          (size_t)in->dst >= value_count || (size_t)in->a >= value_count ||
          (size_t)in->b >= value_count || (size_t)in->c >= value_count ||
          !facts[in->a].known || !facts[in->b].known || !facts[in->c].known) {
        ok = false;
        break;
      }
      facts[in->dst] = facts[in->a].value != 0 ? facts[in->b] : facts[in->c];
      break;
    case NYIR_BR:
    case NYIR_BR_IF: {
      bool take = true;
      if (in->op == NYIR_BR_IF) {
        if (in->a < 0 || (size_t)in->a >= value_count || !facts[in->a].known) {
          ok = false;
          break;
        }
        take = facts[in->a].value != 0;
      }
      predecessor_label = current_label;
      has_predecessor = true;
      if (take) {
        size_t target = SIZE_MAX;
        for (size_t k = 0; k < callee->len; ++k)
          if (callee->data[k].op == NYIR_LABEL &&
              callee->data[k].imm == in->imm) {
            target = k;
            break;
          }
        if (target == SIZE_MAX)
          ok = false;
        else
          pc = target;
      }
      break;
    }
    case NYIR_RET:
      if (in->a < 0 || (size_t)in->a >= value_count || !facts[in->a].known) {
        ok = false;
        break;
      }
      return_value = facts[in->a].value;
      found_return = true;
      break;
    default:
      if (in->op == NYIR_CALL) {
        if (!in->symbol || nyr_find_inline_callee(in->symbol) != callee ||
            in->dst < 0 || (size_t)in->dst >= value_count || in->imm < 0 ||
            in->imm > NYIR_CALL_MAX_ARGS ||
            (size_t)in->imm != callee->param_count) {
          ok = false;
          break;
        }
        int call_args[NYIR_CALL_MAX_ARGS];
        int call_argc = 0;
        int64_t call_values[NYIR_CALL_MAX_ARGS];
        if (!nyr_call_args(in, callee->next_value, call_args,
                            NYIR_CALL_MAX_ARGS, &call_argc, NULL, 0) ||
            call_argc != in->imm) {
          ok = false;
          break;
        }
        for (int k = 0; k < call_argc; ++k) {
          if (call_args[k] < 0 || (size_t)call_args[k] >= value_count ||
              !facts[call_args[k]].known) {
            ok = false;
            break;
          }
          call_values[k] = facts[call_args[k]].value;
        }
        if (ok && !nyr_ipa_eval_callee(callee, call_values, (size_t)call_argc,
                                        &value, instruction_budget, NULL,
                                        recursion_depth + 1u))
          ok = false;
        if (ok)
          facts[in->dst] = (nyr_ipa_const_fact_t){true, value};
      } else if (nyr_ipa_i64_unop(in->op)) {
        if (in->dst < 0 || in->a < 0 || (size_t)in->dst >= value_count ||
            (size_t)in->a >= value_count || !facts[in->a].known) {
          ok = false;
          break;
        }
        uint64_t raw = (uint64_t)facts[in->a].value;
        value = in->op == NYIR_BSWAP_I64
                    ? (int64_t)__builtin_bswap64(raw)
                    : (int64_t)(uint32_t)__builtin_bswap32((uint32_t)raw);
        facts[in->dst] = (nyr_ipa_const_fact_t){true, value};
      } else if (nyr_ipa_i64_binop(in->op) || in->op == NYIR_CMP_I64) {
        if (in->dst < 0 || in->a < 0 || in->b < 0 ||
            (size_t)in->dst >= value_count || (size_t)in->a >= value_count ||
            (size_t)in->b >= value_count || !facts[in->a].known ||
            !facts[in->b].known ||
            !nyr_ipa_const_eval(in->op, facts[in->a].value, facts[in->b].value,
                                 in->cmp, &value)) {
          ok = false;
          break;
        }
        facts[in->dst] = (nyr_ipa_const_fact_t){true, value};
      } else {
        ok = false;
      }
      break;
    }
  }
  free(facts);
  free(locals);
  if (!ok || !found_return)
    return false;
  *result = return_value;
  return true;
}

static bool nyr_ipa_collect_constants(const nyr_func_t *caller,
                                       nyr_ipa_const_fact_t *facts) {
  if (!caller || !facts)
    return false;
  for (size_t i = 0; i < caller->len; ++i) {
    const nyr_inst_t *in = &caller->data[i];
    if (in->dst < 0 || (size_t)in->dst >= (size_t)caller->next_value)
      continue;
    int64_t value = 0;
    if (in->op == NYIR_CONST_I64) {
      facts[in->dst] = (nyr_ipa_const_fact_t){true, in->imm};
    } else if (in->op == NYIR_COPY && in->a >= 0 &&
               (size_t)in->a < (size_t)caller->next_value &&
               facts[in->a].known) {
      facts[in->dst] = facts[in->a];
    } else if (in->op == NYIR_SELECT_I64 && in->a >= 0 && in->b >= 0 &&
               in->c >= 0 && (size_t)in->a < (size_t)caller->next_value &&
               (size_t)in->b < (size_t)caller->next_value &&
               (size_t)in->c < (size_t)caller->next_value &&
               facts[in->a].known && facts[in->b].known && facts[in->c].known) {
      facts[in->dst] = facts[in->a].value != 0 ? facts[in->b] : facts[in->c];
    } else if (nyr_ipa_i64_unop(in->op) && in->a >= 0 &&
               (size_t)in->a < (size_t)caller->next_value &&
               facts[in->a].known) {
      uint64_t raw = (uint64_t)facts[in->a].value;
      value = in->op == NYIR_BSWAP_I64
                  ? (int64_t)__builtin_bswap64(raw)
                  : (int64_t)(uint32_t)__builtin_bswap32((uint32_t)raw);
      facts[in->dst] = (nyr_ipa_const_fact_t){true, value};
    } else if ((nyr_ipa_i64_binop(in->op) || in->op == NYIR_CMP_I64) &&
               in->a >= 0 && in->b >= 0 &&
               (size_t)in->a < (size_t)caller->next_value &&
               (size_t)in->b < (size_t)caller->next_value &&
               facts[in->a].known && facts[in->b].known &&
               nyr_ipa_const_eval(in->op, facts[in->a].value,
                                   facts[in->b].value, in->cmp, &value)) {
      facts[in->dst] = (nyr_ipa_const_fact_t){true, value};
    }
  }
  return true;
}

bool nyr_ipa_constprop(nyr_func_t *f) {
  if (!f || f->len == 0 || f->next_value <= 0 || nyr_inline_callee_count == 0)
    return true;
  nyr_ipa_const_fact_t *facts = calloc((size_t)f->next_value, sizeof(*facts));
  if (!facts)
    return false;
  nyr_ipa_collect_constants(f, facts);
  /*
   * Bound total compile-time interpretation for this function, not each call
   * site independently. Deep or numerous recursions then fall back to the
   * ordinary call path without consuming unbounded optimizer time.
   */
  size_t eval_budget = 100000;
  for (size_t i = 0; i < f->len; ++i) {
    nyr_inst_t *in = &f->data[i];
    if (in->op != NYIR_CALL || in->dst < 0 || !in->symbol ||
        (in->flags & (NYIR_INST_F_EXTERN | NYIR_INST_F_SRET)))
      continue;
    const nyr_func_t *callee = nyr_find_inline_callee(in->symbol);
    if (!callee || callee == f || in->imm < 0 ||
        in->imm != (int64_t)callee->param_count || in->imm > NYIR_CALL_MAX_ARGS)
      continue;
    bool pure_body = nyr_ipa_body_is_scalar_pure(callee);
    bool formal_store_body =
        !pure_body && nyr_ipa_body_is_scalar_with_formal_stores(callee);
    if ((!pure_body && !formal_store_body) ||
        (pure_body && nyr_effective_effects(in) != NYIR_EFFECT_CALL) ||
        (formal_store_body &&
         (!nyr_ipa_effectful_result_summary_matches(in, callee) ||
          (i + 1 < f->len && f->data[i + 1].op == NYIR_CAPTURE_RET))))
      continue;
    int args[NYIR_CALL_MAX_ARGS];
    int argc = 0;
    if (!nyr_call_args(in, f->next_value, args, NYIR_CALL_MAX_ARGS, &argc,
                        NULL, 0) ||
        argc != in->imm)
      continue;
    int64_t arg_values[NYIR_CALL_MAX_ARGS];
    bool args_ok = true;
    for (int j = 0; j < argc; ++j) {
      if (args[j] < 0 || (size_t)args[j] >= (size_t)f->next_value ||
          !facts[args[j]].known) {
        args_ok = false;
        break;
      }
      arg_values[j] = facts[args[j]].value;
    }
    if (!args_ok)
      continue;
    int64_t result = 0;
    nyr_ipa_memory_writes_t memory_writes = {0};
    if (!nyr_ipa_eval_callee(callee, arg_values, (size_t)argc, &result,
                              &eval_budget,
                              formal_store_body ? &memory_writes : NULL, 0))
      continue;
    int forwarded_load_dst = -1;
    int64_t forwarded_load_value = 0;
    if (formal_store_body && memory_writes.complete && i + 1 < f->len &&
        f->data[i + 1].op == NYIR_LOAD_I64 && f->data[i + 1].dst >= 0 &&
        f->data[i + 1].a >= 0 &&
        (size_t)f->data[i + 1].a < (size_t)f->next_value &&
        facts[f->data[i + 1].a].known) {
      const nyr_inst_t *load = &f->data[i + 1];
      const unsigned modeled = NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_MAY_TRAP;
      if ((nyr_effective_effects(load) & ~modeled) == 0) {
        int64_t load_address = facts[load->a].value;
        for (size_t w = 0; w < memory_writes.count; ++w) {
          if (memory_writes.writes[w].address != load_address)
            continue;
          forwarded_load_dst = load->dst;
          forwarded_load_value = memory_writes.writes[w].value;
          break;
        }
      }
    }
    int dst = in->dst;
    const char *symbol = in->symbol;
    nyr_debug_loc_t debug = in->debug;
    nyr_scalar_class_t scalar_class = in->scalar_class;
    uint8_t semantic_ownership = in->semantic_ownership;
    bool semantic_mutable = in->semantic_mutable;
    if (formal_store_body) {
      /*
       * Keep the effectful call as the memory/trap barrier, but separate its
       * scalar result into a constant definition. This only applies when the
       * bounded body proof established that writes target unchanged pointer
       * formals and no memory read can influence the return.
       */
      if (!nyr_ensure_insert(f, i + 1, 1)) {
        free(facts);
        return false;
      }
      f->data[i].dst = -1;
      f->data[i + 1] = (nyr_inst_t){
          .op = NYIR_CONST_I64,
          .dst = dst,
          .a = -1,
          .b = -1,
          .c = -1,
          .d = -1,
          .e = -1,
          .f = -1,
          .imm = result,
          .scalar_class = scalar_class,
          .semantic_ownership = semantic_ownership,
          .semantic_mutable = semantic_mutable,
          .debug = debug,
          .range = {
              .has_min = true, .has_max = true, .min = result, .max = result}};
      if (forwarded_load_dst >= 0) {
        nyr_inst_t *load = &f->data[i + 2];
        if (load->op != NYIR_LOAD_I64 || load->dst != forwarded_load_dst) {
          free(facts);
          return false;
        }
        nir_make_const(load, forwarded_load_value);
        facts[forwarded_load_dst] =
            (nyr_ipa_const_fact_t){true, forwarded_load_value};
      }
    } else {
      free(in->extra_args);
      free(in->arg_sizes);
      free(in->phi_incoming);
      nir_make_const(in, result);
    }
    facts[dst] = (nyr_ipa_const_fact_t){true, result};
    if (ny_trace_enabled("NY_TRACE_IPA_CP"))
      ny_trace_line("IPA_CP", "constant-propagate: %s -> %" PRId64, symbol,
                    result);
  }
  free(facts);
  return true;
}

/*
 * Out-of-line verdicts for runtime builtins and externs are structural
 * facts, not decisions worth a line each: aggregate them per function and
 * print one summary when the function ends (or when an inlined call site
 * reports).  Set NYTRIX_INLINE_TRACE_ALL=1 for the full per-site log.
 */
static size_t nyr_inline_ool_count;
static const char *nyr_inline_ool_last_reason;

static void nyr_inline_ool_flush(const char *fn_name);
static void nyr_inline_small_trace(size_t pc, const char *symbol,
                                    const char *action, const char *reason) {
  if (!ny_dbg_is_enabled(NY_DBG_HEURISTIC))
    return;
  if (action && strcmp(action, "keep-out-of-line") == 0) {
    nyr_inline_ool_count++;
    nyr_inline_ool_last_reason = reason;
    return;
  }
  nyr_inline_ool_flush("<fn>");
  ny_dbg_heuristic_event("INLINE_SMALL", "%s pc=%zu callee=%s reason=%s",
                         action, pc, symbol ? symbol : "<unknown>",
                         reason ? reason : "none");
}

static void nyr_inline_ool_flush(const char *fn_name) {
  if (!nyr_inline_ool_count)
    return;
  ny_dbg_heuristic_event(
      "INLINE_SMALL",
      "%s: %zu calls stay out of line "
      "(last reason=%s; NYTRIX_INLINE_TRACE_ALL=1 logs "
      "each site)",
      fn_name ? fn_name : "<fn>", nyr_inline_ool_count,
      nyr_inline_ool_last_reason ? nyr_inline_ool_last_reason : "none");
  nyr_inline_ool_count = 0;
  nyr_inline_ool_last_reason = NULL;
}

/*
 * Inlining replaces a call-site summary with the effects of its cloned body.
 * A verified summary can be more conservative than that body (for example,
 * because it includes argument-reachable memory), so inline only when every
 * advertised non-call effect remains represented after cloning. Calls with
 * no verified summary retain the existing body-based legality checks.
 */
static bool nyr_inline_summary_covered(const nyr_inst_t *call,
                                        const nyr_func_t *callee) {
  if (!call || !callee || (call->flags & NYIR_INST_F_EFFECTS_KNOWN) == 0)
    return true;
  unsigned body_effects = 0;
  for (size_t i = 0; i < callee->len; ++i)
    body_effects |= nyr_effective_effects(&callee->data[i]);
  unsigned unrepresented = call->effects & ~(body_effects | NYIR_EFFECT_CALL);
  return unrepresented == 0;
}

bool nyr_inline_small(nyr_func_t *f) {
  if (!f || nyr_inline_callee_count == 0)
    return true;
  for (size_t i = 0; i < f->len; ++i) {
    nyr_inst_t *in = &f->data[i];
    if (in->op != NYIR_CALL || !in->symbol)
      continue;
    /*
     * An extern may share its spelling with a Nytrix helper (for example
     * libc's `div`).  Its ABI and side effects belong to the foreign
     * declaration; never inline a same-named local body into it.
     */
    if (in->flags & NYIR_INST_F_EXTERN) {
      nyr_inline_small_trace(i, in->symbol, "keep-out-of-line",
                              "foreign ABI call");
      continue;
    }
    const nyr_func_t *callee = nyr_find_inline_callee(in->symbol);
    if (!callee || !nyr_func_is_inline_candidate(callee)) {
      nyr_inline_small_trace(i, in->symbol, "keep-out-of-line",
                              callee ? "callee is not a small-inliner candidate"
                                     : "no registered monomorphic callee body");
      continue;
    }
    if (!nyr_inline_summary_covered(in, callee)) {
      nyr_inline_small_trace(i, in->symbol, "keep-out-of-line",
                              "call-site effects exceed cloned body summary");
      continue;
    }
    size_t callee_ret_count = 0;
    for (size_t k = 0; k < callee->len; ++k)
      callee_ret_count += callee->data[k].op == NYIR_RET;

    /*
     * Constant-return fast path: replace the call with the constant ONLY
     * when EVERY return in the callee yields that SAME constant and the
     * body contains nothing observable (no calls, stores, or trapping
     * ops). The previous shape fired when ANY ret was constant, so a
     * callee like `if c { return 1 } expensive()` folded to 1 on every
     * path and silently dropped the other return's value.
     */
    int64_t ret_imm = 0;
    bool ret_const = true;
    bool saw_ret = false;
    for (size_t k = 0; k < callee->len && ret_const; ++k) {
      const nyr_inst_t *c = &callee->data[k];
      if (c->op == NYIR_RET) {
        if (c->a < 0) {
          ret_const = false;
          break;
        }
        int64_t this_imm = 0;
        bool this_const = false;
        for (size_t j = 0; j < k; ++j) {
          if (callee->data[j].op == NYIR_CONST_I64 &&
              callee->data[j].dst == c->a) {
            this_imm = callee->data[j].imm;
            this_const = true;
          }
        }
        if (!this_const || (saw_ret && this_imm != ret_imm)) {
          ret_const = false;
          break;
        }
        ret_imm = this_imm;
        saw_ret = true;
        continue;
      }
      switch (c->op) {
      case NYIR_NOP:
      case NYIR_LABEL:
      case NYIR_BR:
      case NYIR_BR_IF:
      case NYIR_CONST_I64:
      case NYIR_COPY:
        continue;
      default:
        ret_const = false;
        break;
      }
    }
    if (ret_const && saw_ret && in->dst >= 0) {
      nyr_inline_small_trace(i, in->symbol, "accept",
                              "all returns share one unobservable constant");
      free(in->extra_args);
      free(in->arg_sizes);
      free(in->phi_incoming);
      nir_make_const(in, ret_imm);
      continue;
    }

    /*
     * Single-expression callees: load.local 0; op; ret — specialize when
     * one arg and pure unary/binary on that arg only.
     */
    if (callee->len == 3 && in->a >= 0 && in->dst >= 0 &&
        callee->data[0].op == NYIR_LOAD_LOCAL && callee->data[0].imm == 0 &&
        callee->data[2].op == NYIR_RET &&
        callee->data[2].a == callee->data[1].dst) {
      const nyr_inst_t *op = &callee->data[1];
      if (nyr_is_i64_binop(op->op)) {
        /*
         * Need const second operand in callee.
         */
        int64_t imm = 0;
        bool has_imm = false;
        if (op->a == callee->data[0].dst && op->b >= 0) {
          for (size_t j = 0; j < callee->len; ++j)
            if (callee->data[j].op == NYIR_CONST_I64 &&
                callee->data[j].dst == op->b) {
              imm = callee->data[j].imm;
              has_imm = true;
            }
        }
        if (has_imm) {
          /*
           * Expand call to: const imm; op arg, imm → dst. Capture all CALL
           * fields before nyr_ensure_insert(): it may realloc f->data and
           * invalidate the `in` pointer.
           */
          int adst = in->dst;
          int arg = in->a;
          nyr_op_t oop = op->op;
          nyr_inline_small_trace(
              i, in->symbol, "accept",
              "single-parameter operation with constant operand");
          if (!nyr_ensure_insert(f, i, 1))
            return false;
          int cdst = f->next_value++;
          f->data[i] = (nyr_inst_t){
              .op = NYIR_CONST_I64,
              .dst = cdst,
              .imm = imm,
              .range = {
                  .has_min = true, .has_max = true, .min = imm, .max = imm}};
          f->data[i + 1] =
              (nyr_inst_t){.op = oop, .dst = adst, .a = arg, .b = cdst};
          i++;
          continue;
        }
      }
    }

    /*
     * Generic two-parameter binary operation callee inlining:
     * trace return value back to an operation on parameters (add, mul, cmp,
     * etc.)
     */
    int b_op = -1, b_p0 = -1, b_p1 = -1;
    nyr_cmp_t b_cmp = NYIR_CMP_EQ;
    int ret_val = -1;
    for (size_t k = 0; k < callee->len; ++k) {
      if (callee->data[k].op == NYIR_RET) {
        ret_val = callee->data[k].a;
        break;
      }
    }
    if (ret_val >= 0 && in->dst >= 0) {
      for (int depth = 0; depth < 16; ++depth) {
        int def_i = -1;
        for (size_t k = 0; k < callee->len; ++k) {
          if (callee->data[k].dst == ret_val) {
            def_i = (int)k;
            break;
          }
        }
        if (def_i < 0)
          break;
        const nyr_inst_t *def = &callee->data[def_i];
        if (def->op == NYIR_COPY && def->a >= 0) {
          ret_val = def->a;
        } else if (nyr_is_i64_binop(def->op) || def->op == NYIR_ADD_F64 ||
                   def->op == NYIR_SUB_F64 || def->op == NYIR_MUL_F64 ||
                   def->op == NYIR_DIV_F64 || def->op == NYIR_CMP_I64 ||
                   def->op == NYIR_CMP_F64) {
          int p0 = def->a, p1 = def->b;
          int param0 = (p0 >= 0 && p0 < (int)callee->param_count) ? p0 : -1;
          int param1 = (p1 >= 0 && p1 < (int)callee->param_count) ? p1 : -1;
          for (size_t k = 0; k < callee->len; ++k) {
            const nyr_inst_t *c = &callee->data[k];
            if (c->dst == p0 && c->op == NYIR_LOAD_LOCAL && c->imm >= 0 &&
                c->imm < (int64_t)callee->param_count)
              param0 = (int)c->imm;
            if (c->dst == p1 && c->op == NYIR_LOAD_LOCAL && c->imm >= 0 &&
                c->imm < (int64_t)callee->param_count)
              param1 = (int)c->imm;
            if (c->dst == p0 && c->op == NYIR_COPY && c->a >= 0 &&
                c->a < (int)callee->param_count)
              param0 = c->a;
            if (c->dst == p1 && c->op == NYIR_COPY && c->a >= 0 &&
                c->a < (int)callee->param_count)
              param1 = c->a;
          }
          if (param0 >= 0 && param1 >= 0) {
            b_op = def->op;
            b_p0 = param0;
            b_p1 = param1;
            b_cmp = def->cmp;
          }
          break;
        } else
          break;
      }
    }
    if (callee_ret_count == 1 && b_op >= 0 && b_p0 >= 0 && b_p1 >= 0 &&
        (in->imm >= 2 || (in->a >= 0 && in->b >= 0))) {
      int call_args[6] = {in->a, in->b, in->c, in->d, in->e, in->f};
      int arg0 = b_p0 < 6 ? call_args[b_p0] : -1;
      int arg1 = b_p1 < 6 ? call_args[b_p1] : -1;
      if (arg0 >= 0 && arg1 >= 0) {
        nyr_inline_small_trace(
            i, in->symbol, "accept",
            "single pure operation over two formal parameters");
        *in = (nyr_inst_t){.op = (nyr_op_t)b_op,
                            .dst = in->dst,
                            .a = arg0,
                            .b = arg1,
                            .cmp = b_cmp};
        continue;
      }
    }

    /*
     * Three-inst with const in middle: load0; const; binop; ret (len 4).
     */
    if (callee->len == 4 && in->a >= 0 && in->dst >= 0 &&
        callee->data[0].op == NYIR_LOAD_LOCAL && callee->data[0].imm == 0 &&
        callee->data[1].op == NYIR_CONST_I64 &&
        nyr_is_i64_binop(callee->data[2].op) &&
        callee->data[2].a == callee->data[0].dst &&
        callee->data[2].b == callee->data[1].dst &&
        callee->data[3].op == NYIR_RET &&
        callee->data[3].a == callee->data[2].dst) {
      int adst = in->dst;
      int arg = in->a;
      int64_t imm = callee->data[1].imm;
      nyr_op_t oop = callee->data[2].op;
      nyr_inline_small_trace(
          i, in->symbol, "accept",
          "three-instruction body with constant operation operand");
      if (!nyr_ensure_insert(f, i, 1))
        return false;
      int cdst = f->next_value++;
      f->data[i] = (nyr_inst_t){
          .op = NYIR_CONST_I64,
          .dst = cdst,
          .imm = imm,
          .range = {.has_min = true, .has_max = true, .min = imm, .max = imm}};
      f->data[i + 1] =
          (nyr_inst_t){.op = oop, .dst = adst, .a = arg, .b = cdst};
      i++;
      continue;
    }

    /*
     * General pure body splice (≤12 insts): map load.local p → arg, remap
     * temps, replace CALL with inlined stream ending in COPY to call dst.
     */
    if (callee_ret_count == 1 && callee->len <= 14 && callee->len >= 2 &&
        in->dst >= 0) {
      int args[6] = {in->a, in->b, in->c, in->d, in->e, in->f};
      int arity = (int)in->imm;
      if (arity < 0)
        arity = 0;
      if (arity > 6)
        arity = 6;
      bool ok_s = true;
      int max_loc = -1;
      for (size_t k = 0; k < callee->len; ++k) {
        const nyr_inst_t *c = &callee->data[k];
        if (c->op == NYIR_NOP || c->op == NYIR_RET || c->op == NYIR_LABEL)
          continue;
        if (c->op == NYIR_LOAD_LOCAL) {
          if (c->imm > max_loc)
            max_loc = (int)c->imm;
          continue;
        }
        if (c->op == NYIR_STORE_LOCAL) {
          ok_s = false;
          break;
        }
        if (c->op == NYIR_CONST_I64 || c->op == NYIR_CONST_F64 ||
            c->op == NYIR_COPY || nyr_is_i64_binop(c->op) ||
            c->op == NYIR_ADD_F64 || c->op == NYIR_SUB_F64 ||
            c->op == NYIR_MUL_F64 || c->op == NYIR_DIV_F64 ||
            c->op == NYIR_CMP_I64 || c->op == NYIR_CMP_F64)
          continue;
        ok_s = false;
        break;
      }
      if (ok_s && max_loc < arity) {
        size_t body_n = 0;
        for (size_t k = 0; k < callee->len; ++k)
          if (callee->data[k].op != NYIR_RET &&
              callee->data[k].op != NYIR_NOP &&
              callee->data[k].op != NYIR_LABEL)
            body_n++;
        if (body_n > 0 && body_n <= 12) {
          /*
           * nyr_ensure_insert below may move f->data.  Preserve the CALL
           * destination now rather than dereferencing `in` afterward.
           */
          int call_dst = in->dst;
          nyr_inline_small_trace(
              i, in->symbol, "accept",
              "pure straight-line body fits 12-instruction splice limit");
          int *vmap = NULL;
          if (callee->next_value > 0) {
            vmap = calloc((size_t)callee->next_value, sizeof(int));
            if (!vmap)
              return false;
            for (int v = 0; v < callee->next_value; ++v)
              vmap[v] = -1;
          }
          if (!nyr_ensure_insert(f, i, body_n)) {
            free(vmap);
            return false;
          }
          size_t w = i;
          for (size_t k = 0; k < callee->len; ++k) {
            const nyr_inst_t *c = &callee->data[k];
            if (c->op == NYIR_NOP || c->op == NYIR_LABEL)
              continue;
            if (c->op == NYIR_RET) {
              int src = -1;
              if (c->a >= 0 && vmap && c->a < callee->next_value &&
                  vmap[c->a] >= 0)
                src = vmap[c->a];
              else if (c->a >= 0 && c->a < arity)
                src = args[c->a];
              if (src >= 0)
                f->data[w++] = (nyr_inst_t){
                    .op = NYIR_COPY, .dst = call_dst, .a = src, .b = -1};
              continue;
            }
            if (c->op == NYIR_LOAD_LOCAL && c->imm >= 0 &&
                (int)c->imm < arity) {
              int nd = f->next_value++;
              if (vmap && c->dst >= 0 && c->dst < callee->next_value)
                vmap[c->dst] = nd;
              f->data[w++] = (nyr_inst_t){
                  .op = NYIR_COPY, .dst = nd, .a = args[c->imm], .b = -1};
              continue;
            }
            nyr_inst_t ni = *c;
            if (ni.dst >= 0) {
              int nd = f->next_value++;
              if (vmap && ni.dst < callee->next_value)
                vmap[ni.dst] = nd;
              ni.dst = nd;
            }
            if (ni.a >= 0 && vmap && ni.a < callee->next_value &&
                vmap[ni.a] >= 0)
              ni.a = vmap[ni.a];
            if (ni.b >= 0 && vmap && ni.b < callee->next_value &&
                vmap[ni.b] >= 0)
              ni.b = vmap[ni.b];
            f->data[w++] = ni;
          }
          for (size_t z = w; z <= i + body_n && z < f->len; ++z)
            f->data[z] =
                (nyr_inst_t){.op = NYIR_NOP, .dst = -1, .a = -1, .b = -1};
          free(vmap);
          i = w > 0 ? w - 1 : i;
          continue;
        }
      }
    }
    nyr_inline_small_trace(
        i, in->symbol, "keep-out-of-line",
        "no supported constant, parameter-op, or pure-splice shape matched");
  }
  return true;
}

/*
 * Opt-in inliner remarks.  Keep explanation work off the optimizer fast path
 * unless explicitly requested so normal compilation does not allocate or
 * format diagnostics.
 */
static bool nyr_inline_trace_enabled(void) {
  return ny_dbg_is_enabled(NY_DBG_HEURISTIC) ||
         ny_trace_enabled("NY_TRACE_INLINE");
}

static void nyr_inline_trace(const nyr_func_t *caller, size_t call_idx,
                              const char *callee, const char *status,
                              const char *reason) {
  if (!nyr_inline_trace_enabled())
    return;
  const nyr_inst_t *call =
      caller && call_idx < caller->len ? &caller->data[call_idx] : NULL;
  const char *file = call && call->debug.file ? call->debug.file : "<unknown>";
  unsigned line = call ? call->debug.line : 0;
  unsigned column = call ? call->debug.column : 0;
  if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
    ny_dbg_heuristic_event(
        "INLINE", "%s pc=%zu callee=%s at %s:%u:%u reason=%s",
        status ? status : "remark", call_idx, callee ? callee : "<unknown>",
        file, line, column, reason ? reason : "none");
  else
    ny_trace_line("INLINE", "inline: %s pc=%zu callee=%s at %s:%u:%u reason=%s",
                  status ? status : "remark", call_idx,
                  callee ? callee : "<unknown>", file, line, column,
                  reason ? reason : "none");
}

static bool nyr_inline_value_is_const(const nyr_func_t *f, size_t before,
                                       int value) {
  if (!f || value < 0)
    return false;
  if (before > f->len)
    before = f->len;
  for (size_t i = before; i-- > 0;) {
    const nyr_inst_t *in = &f->data[i];
    if (in->dst != value)
      continue;
    return in->op == NYIR_CONST_I64 || in->op == NYIR_CONST_F64 ||
           in->op == NYIR_CONST_F32;
  }
  return false;
}

/*
 * Cheap structural loop-depth estimate for profitability.  Canonical NYIR
 * loops carry a backward branch to an earlier label; count the intervals that
 * contain the call.  This is deliberately only a cost-model hint: legality
 * and correctness never depend on the estimate, and deeply nested credit is
 * capped by the caller below.
 */
static size_t nyr_inline_static_loop_depth_slow(const nyr_func_t *f,
                                                 size_t call_idx) {
  if (!f || call_idx >= f->len)
    return 0;
  size_t depth = 0;
  for (size_t i = call_idx + 1; i < f->len; ++i) {
    const nyr_inst_t *term = &f->data[i];
    if (term->op != NYIR_BR && term->op != NYIR_BR_IF)
      continue;
    size_t target = SIZE_MAX;
    for (size_t j = 0; j <= call_idx; ++j) {
      if (f->data[j].op == NYIR_LABEL && f->data[j].imm == term->imm) {
        target = j;
        break;
      }
    }
    if (target != SIZE_MAX && target <= call_idx) {
      ++depth;
      if (depth == 3)
        break;
    }
  }
  return depth;
}

typedef struct {
  int64_t label;
  size_t index;
  bool used;
} nyr_inline_label_slot_t;

static uint64_t nyr_inline_label_hash(int64_t label) {
  uint64_t hash = (uint64_t)label;
  hash ^= hash >> 30;
  hash *= UINT64_C(0xbf58476d1ce4e5b9);
  hash ^= hash >> 27;
  hash *= UINT64_C(0x94d049bb133111eb);
  return hash ^ (hash >> 31);
}

/* Build the same capped backward-branch depth hint for every instruction in
 * one linear pass. Call-site queries then become O(1) instead of rescanning
 * every branch and label in the function. */
static bool nyr_inline_compute_loop_depths(const nyr_func_t *f,
                                            uint8_t **out_depths) {
  if (!out_depths)
    return false;
  *out_depths = NULL;
  if (!f || f->len == 0)
    return true;
  if (f->len > SIZE_MAX / 2u || f->len == SIZE_MAX)
    return false;

  size_t capacity = 16;
  while (capacity < f->len * 2u) {
    if (capacity > SIZE_MAX / 2u)
      return false;
    capacity *= 2u;
  }
  nyr_inline_label_slot_t *labels = calloc(capacity, sizeof(*labels));
  size_t *starts = calloc(f->len + 1u, sizeof(*starts));
  size_t *ends = calloc(f->len + 1u, sizeof(*ends));
  uint8_t *depths = calloc(f->len, sizeof(*depths));
  if (!labels || !starts || !ends || !depths) {
    free(labels);
    free(starts);
    free(ends);
    free(depths);
    return false;
  }

  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *inst = &f->data[i];
    if (inst->op != NYIR_LABEL)
      continue;
    size_t slot = (size_t)nyr_inline_label_hash(inst->imm) & (capacity - 1u);
    while (labels[slot].used && labels[slot].label != inst->imm)
      slot = (slot + 1u) & (capacity - 1u);
    if (!labels[slot].used)
      labels[slot] = (nyr_inline_label_slot_t){inst->imm, i, true};
  }

  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *term = &f->data[i];
    if (term->op != NYIR_BR && term->op != NYIR_BR_IF)
      continue;
    size_t slot = (size_t)nyr_inline_label_hash(term->imm) & (capacity - 1u);
    while (labels[slot].used && labels[slot].label != term->imm)
      slot = (slot + 1u) & (capacity - 1u);
    if (!labels[slot].used || labels[slot].index > i)
      continue;
    ++starts[labels[slot].index];
    ++ends[i];
  }

  size_t active = 0;
  for (size_t i = 0; i < f->len; ++i) {
    active += starts[i];
    active -= ends[i];
    depths[i] = active >= 3u ? 3u : (uint8_t)active;
  }
  free(labels);
  free(starts);
  free(ends);
  *out_depths = depths;
  return true;
}

typedef uint32_t nyr_inline_expansion_t;
#define NYIR_INLINE_NO_EXPANSION UINT32_MAX

/*
 * Immutable ancestry nodes replace the old fixed six-symbol expansion stack.
 * An instruction stores only a node index; descendants append one node and
 * share their parent's prefix.  Exact cycle detection is therefore O(depth)
 * with no arbitrary call-chain depth limit, while the pass-wide splice budget
 * below remains the sole code-growth bound.
 */
typedef struct {
  const char *symbol;
  size_t parent;
} nyr_inline_chain_node_t;

typedef struct {
  nyr_inline_chain_node_t *data;
  size_t len;
  size_t cap;
} nyr_inline_chain_t;

static bool nyr_inline_expansion_allows(const nyr_inline_chain_t *chain,
                                         nyr_inline_expansion_t expansion,
                                         const char *callee_symbol) {
  if (!chain || !callee_symbol)
    return false;
  for (size_t at = expansion; at != NYIR_INLINE_NO_EXPANSION;) {
    if (at >= chain->len)
      return false;
    const nyr_inline_chain_node_t *node = &chain->data[at];
    if (!node->symbol || strcmp(node->symbol, callee_symbol) == 0)
      return false;
    at = node->parent;
  }
  return true;
}

static bool nyr_inline_chain_push(nyr_inline_chain_t *chain,
                                   nyr_inline_expansion_t parent,
                                   const char *symbol,
                                   nyr_inline_expansion_t *out) {
  if (!chain || !symbol || !out)
    return false;
  if (chain->len >= NYIR_INLINE_NO_EXPANSION)
    return false;
  if (chain->len == chain->cap) {
    size_t next = chain->cap ? chain->cap * 2u : 16u;
    if (next < chain->cap)
      return false;
    nyr_inline_chain_node_t *data =
        ny_realloc_array(chain->data, next, sizeof(*data));
    if (!data)
      return false;
    chain->data = data;
    chain->cap = next;
  }
  *out = chain->len;
  chain->data[chain->len++] =
      (nyr_inline_chain_node_t){.symbol = symbol, .parent = parent};
  return true;
}

static bool nyr_inline_expansion_reserve(nyr_inline_expansion_t **expansions,
                                          size_t *capacity, size_t needed) {
  if (needed <= *capacity)
    return true;
  size_t next_capacity = *capacity ? *capacity : 16;
  while (next_capacity < needed) {
    if (next_capacity > SIZE_MAX / 2) {
      next_capacity = needed;
      break;
    }
    next_capacity *= 2;
  }
  nyr_inline_expansion_t *next =
      ny_realloc_array(*expansions, next_capacity, sizeof(*next));
  if (!next)
    return false;
  *expansions = next;
  *capacity = next_capacity;
  return true;
}

static void nyr_inline_expansion_splice(nyr_inline_expansion_t *expansions,
                                         size_t old_len, size_t call_idx,
                                         size_t inserted,
                                         nyr_inline_expansion_t child) {
  memmove(&expansions[call_idx + inserted], &expansions[call_idx + 1],
          (old_len - call_idx - 1) * sizeof(*expansions));
  for (size_t i = 0; i < inserted; ++i)
    expansions[call_idx + i] = child;
}

/*
 * Cost model for inlining.  Profile heat raises the size budget, constants and
 * optimization-exposing operations add a bounded benefit credit, while a high
 * temporary-value count approximates post-inline register-pressure growth.
 */
static bool nyr_should_inline_general(
    const nyr_func_t *caller, const nyr_inst_t *call,
    const nyr_func_t *callee, size_t call_idx,
    const nyr_inline_chain_t *chain, nyr_inline_expansion_t expansion,
    size_t static_loop_depth, const char *callee_symbol) {
  const char *symbol = call ? call->symbol : NULL;
  if (!caller || !call || !callee || callee == caller || callee->len == 0 ||
      call->imm < 0 || call->imm > NYIR_CALL_MAX_ARGS ||
      (call->flags & (NYIR_INST_F_SRET | NYIR_INST_F_EXTERN))) {
    nyr_inline_trace(caller, call_idx, symbol, "reject",
                      "unsupported call shape or recursive/sret callee");
    return false;
  }
  const nyr_inline_callee_summary_t *summary =
      nyr_find_inline_callee_summary(callee_symbol);

  if (!nyr_inline_expansion_allows(chain, expansion, callee_symbol)) {
    nyr_inline_trace(caller, call_idx, symbol, "reject",
                      "symbol-recursive inline expansion");
    return false;
  }

  /*
   * Keep a directly recursive function out of callers.  Expansion ancestry
   * prevents recursively cloning the same symbol, but allowing one clone of
   * a recursive body still duplicates its local/return CFG at every call
   * site.  That is both unprofitable and has caused incorrect values across
   * cloned recursive control-flow paths.  The recursive function remains a
   * normal call, preserving its established ABI and stack behavior.
   */
  if (summary && summary->direct_recursive) {
    nyr_inline_trace(caller, call_idx, symbol, "reject",
                      "directly recursive callee remains out of line");
    return false;
  } else if (!summary) {
    for (size_t i = 0; i < callee->len; ++i) {
      const nyr_inst_t *inst = &callee->data[i];
      if (inst->op == NYIR_CALL && inst->symbol &&
          strcmp(inst->symbol, callee_symbol) == 0) {
        nyr_inline_trace(caller, call_idx, symbol, "reject",
                          "directly recursive callee remains out of line");
        return false;
      }
    }
  }

  uint64_t block_heat = ny_native_profile_block_hot(call_idx);
  uint64_t loop_heat = ny_native_profile_loop_hot(call_idx);
  uint64_t edge_heat = ny_native_profile_edge_hot(call_idx, call_idx + 1);
  bool hot = (ny_native_profile_steps() > 10000 && edge_heat > 100) ||
             (ny_native_profile_steps() > 1000 && block_heat > 50) ||
             loop_heat >= 32;
  size_t max_callee_size = hot ? 256 : 192;

  int args[NYIR_CALL_MAX_ARGS];
  int arity = 0;
  if (!nyr_call_args(call, caller->next_value, args, NYIR_CALL_MAX_ARGS,
                      &arity, NULL, 0)) {
    nyr_inline_trace(caller, call_idx, symbol, "reject",
                      "malformed call argument vector");
    return false;
  }

  /*
   * Keep functions that consume closure-valued formals behind their native
   * call boundary.  The current clone path substitutes immutable parameter
   * loads directly with caller SSA values, but that is not sufficient for
   * closure formals: their representation couples the closure object to its
   * environment and callback ABI record.  Inlining such a formal can leave a
   * capture environment where later callback dispatch expects the closure
   * object (notably when CPS combinators construct a second captured lambda).
   * Preserve the established parameter ABI until cloning carries the full
   * callable contract explicitly.
   */
  if (summary) {
    bool closure_formal_used = false;
    for (int a = 0; a < arity; ++a)
      if (summary->closure_formals & (UINT64_C(1) << a)) {
        closure_formal_used = true;
        break;
      }
    if (closure_formal_used) {
      nyr_inline_trace(caller, call_idx, symbol, "reject",
                        "closure formal retains its native ABI boundary");
      return false;
    }
    if (summary->nested_closure_call) {
      nyr_inline_trace(caller, call_idx, symbol, "reject",
                        "nested closure ABI call retains its native boundary");
      return false;
    }
  } else {
    for (size_t i = 0; i < callee->len; ++i) {
      const nyr_inst_t *in = &callee->data[i];
      if (in->op == NYIR_LOAD_LOCAL && in->imm >= 0 && in->imm < arity &&
          in->scalar_class == NYIR_SCALAR_CLOSURE) {
        nyr_inline_trace(caller, call_idx, symbol, "reject",
                          "closure formal retains its native ABI boundary");
        return false;
      }
      /*
       * Keep the enclosing call boundary too: cloning a caller that forwards
       * closure values can otherwise detach the closure object from the
       * environment/ABI state consumed by the nested callee.
       */
      if (in->op == NYIR_CALL && in->symbol) {
        const nyr_func_t *target = nyr_find_inline_callee(in->symbol);
        if (target) {
          for (size_t ti = 0; ti < target->len; ++ti) {
            const nyr_inst_t *formal = &target->data[ti];
            if (formal->op == NYIR_LOAD_LOCAL && formal->imm >= 0 &&
                (uint64_t)formal->imm < target->param_count &&
                formal->scalar_class == NYIR_SCALAR_CLOSURE) {
              nyr_inline_trace(
                  caller, call_idx, symbol, "reject",
                  "nested closure ABI call retains its native boundary");
              return false;
            }
          }
        }
      }
    }
  }

  size_t constant_args = 0;
  for (int a = 0; a < arity; ++a)
    if (nyr_inline_value_is_const(caller, call_idx, args[a]))
      ++constant_args;

  /*
   * A call in a statically-recognizable loop pays its overhead every
   * iteration even when runtime profile data is unavailable (the normal
   * ahead-of-time / warm-no-exec benchmark case).  The old 64-instruction
   * ceiling therefore rejected medium-sized loop helpers before later
   * scalarization/BCE/vectorization could see through the call boundary.
   * Keep straight-line cold callers conservative, but give repeated static
   * loop calls the same first-tier budget as a hot call, and a little more
   * for nested loops where the amortized benefit is larger.
   */
  if (static_loop_depth >= 2 && max_callee_size < 384)
    max_callee_size = 384;
  else if (static_loop_depth >= 1 && max_callee_size < 256)
    max_callee_size = 256;

  size_t optimization_credit = (summary ? summary->optimization_credit : 0) +
                               constant_args * 8 + static_loop_depth * 12;
  if (summary && summary->invalid_local) {
    nyr_inline_trace(caller, call_idx, symbol, "reject",
                      "callee has an invalid local-slot reference");
    return false;
  }
  if (!summary) {
    for (size_t k = 0; k < callee->len; ++k) {
      const nyr_inst_t *in = &callee->data[k];
      /*
       * Immutable parameter loads are substituted with caller SSA arguments.
       * Mutable/address-taken formals are legal too: the clone path
       * materializes only those bindings into fresh caller-local slots before
       * entering the cloned body.
       */
      if ((in->op == NYIR_LOAD_LOCAL || in->op == NYIR_STORE_LOCAL ||
           in->op == NYIR_ADDR_LOCAL) &&
          (in->imm < 0 || in->imm >= INT_MAX)) {
        nyr_inline_trace(caller, call_idx, symbol, "reject",
                          "callee has an invalid local-slot reference");
        return false;
      }
      if (in->op == NYIR_ALLOCA)
        optimization_credit += 4;
      else if (in->op == NYIR_BOUNDS_CHECK)
        optimization_credit += 3;
    }
  }
  if (optimization_credit > 64)
    optimization_credit = 64;

  size_t pressure_penalty = 0;
  if (callee->next_value > 32) {
    pressure_penalty = (size_t)(callee->next_value - 32) / 2;
    /*
     * Register pressure still matters, but an unbounded synthetic penalty
     * used to defeat otherwise-profitable repeated-call inlining.
     */
    if (static_loop_depth && pressure_penalty > 64)
      pressure_penalty = 64;
  }
  size_t effective_size = callee->len;
  if (pressure_penalty > SIZE_MAX - effective_size)
    effective_size = SIZE_MAX;
  else
    effective_size += pressure_penalty;

  /*
   * Keep high-pressure control-flow helpers out of repeated loops when their
   * clone expands the caller substantially. The call overhead is amortized,
   * but cloning these CFGs multiplies live values and can overwhelm the
   * native scalar allocator. Deeper loop nests need a tighter ceiling because
   * they multiply the body again; a single loop can tolerate a larger body.
   * Hot-profiled calls continue to use the ordinary hot budget.
   */
  size_t pressure_cfg_limit = static_loop_depth >= 2 ? 96 : 128;
  if (!hot && static_loop_depth >= 1 && callee->next_value > 32 &&
      (summary ? summary->has_control_flow
               : nyr_func_has_control_flow(callee)) &&
      effective_size > pressure_cfg_limit) {
    nyr_inline_trace(caller, call_idx, symbol, "reject",
                      "loop CFG exceeds native pressure budget");
    return false;
  }
  size_t budget = max_callee_size;
  if (optimization_credit <= SIZE_MAX - budget)
    budget += optimization_credit;

  if (nyr_inline_trace_enabled())
    nyr_inline_trace(caller, call_idx, symbol, "cost-model",
                      hot ? "profile says hot" : "profile says cold");
  if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
    ny_dbg_heuristic_event(
        "INLINE_COST",
        "callee=%s pc=%zu hot=%d block_heat=%" PRIu64 " loop_heat=%" PRIu64
        " edge_heat=%" PRIu64
        " loops=%zu constants=%zu size=%zu pressure_penalty=%zu"
        " credit=%zu budget=%zu effective_size=%zu",
        symbol ? symbol : "<unknown>", call_idx, hot, block_heat, loop_heat,
        edge_heat, static_loop_depth, constant_args, callee->len,
        pressure_penalty, optimization_credit, budget, effective_size);

  if (effective_size > budget) {
    nyr_inline_trace(
        caller, call_idx, symbol, "reject",
        "code-size/register-pressure cost exceeds benefit budget");
    return false;
  }

  nyr_inline_trace(
      caller, call_idx, symbol, "cost-approved",
      hot ? "hot call is legal and profitable"
          : (static_loop_depth
                 ? "loop nesting raises repeated-call benefit"
                 : (constant_args ? "constant exposure offsets inline cost"
                                  : "legal callee fits inline budget")));
  return true;
}

static bool nyr_inline_push(nyr_func_t *f, nyr_inst_t in) {
  /*
   * Synthesized control-flow/local instructions must carry the same canonical
   * effect mask as instructions emitted through nyr_ir_emit().  Recompute here
   * so clone/splice helpers cannot accidentally leave stale metadata behind.
   */
  in.effects = nyr_inst_effects(&in);
  if (f->len == f->cap) {
    size_t cap = f->cap ? f->cap * 2 : 32;
    nyr_inst_t *data = ny_realloc_array(f->data, cap, sizeof(*data));
    if (!data)
      return false;
    f->data = data;
    f->cap = cap;
  }
  f->data[f->len++] = in;
  return true;
}

/*
 * Move instruction-owned arrays from a deep-cloned callee into `body`.
 */
static bool nyr_inline_take(nyr_func_t *body, nyr_inst_t *in) {
  if (!nyr_inline_push(body, *in))
    return false;
  in->extra_args = NULL;
  in->extra_args_len = 0;
  in->arg_sizes = NULL;
  in->phi_incoming = NULL;
  in->phi_incoming_len = 0;
  return true;
}

static bool nyr_inline_next_label(const nyr_func_t *f, int64_t *out) {
  int64_t next = 0;
  for (size_t i = 0; i < f->len; ++i) {
    if (f->data[i].op != NYIR_LABEL || f->data[i].imm < next)
      continue;
    if (f->data[i].imm == INT64_MAX)
      return false;
    next = f->data[i].imm + 1;
  }
  *out = next;
  return true;
}

static int nyr_inline_block_for_label(const nyr_cfg_t *cfg, int64_t label) {
  for (size_t b = 0; b < cfg->block_count; ++b)
    if (cfg->block_label[b] == label)
      return (int)b;
  return -1;
}

static int nyr_inline_value(const int *vmap, int n, int value) {
  return value < 0 ? value : (!vmap || value >= n ? -1 : vmap[value]);
}

static int nyr_inline_local_count(const nyr_func_t *f) {
  int max_slot = -1;
  if (!f)
    return 0;
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if ((in->op == NYIR_LOAD_LOCAL || in->op == NYIR_STORE_LOCAL ||
         in->op == NYIR_ADDR_LOCAL) &&
        in->imm >= 0 && in->imm < INT_MAX && in->imm > max_slot)
      max_slot = (int)in->imm;
  }
  return max_slot + 1;
}

/*
 * Recover concrete ABI classes when inlining materializes a formal. A caller
 * value may itself be a local load; accept its class only when every store to
 * that slot agrees, leaving mixed/dynamic locals conservative.
 */
static nyr_scalar_class_t nyr_inline_value_class(const nyr_func_t *f,
                                                   const nyr_type_map_t *types,
                                                   int value, unsigned depth) {
  if (!f || value < 0 || value >= f->next_value || depth > 32)
    return NYIR_SCALAR_UNKNOWN;
  const nyr_inst_t *def = NULL;
  for (size_t i = f->len; i > 0; --i)
    if (f->data[i - 1].dst == value) {
      def = &f->data[i - 1];
      break;
    }
  if (!def)
    return NYIR_SCALAR_UNKNOWN;
  if (def->op == NYIR_LOAD_LOCAL) {
    nyr_scalar_class_t joined = NYIR_SCALAR_UNKNOWN;
    bool saw_store = false;
    for (size_t i = 0; i < f->len; ++i) {
      const nyr_inst_t *store = &f->data[i];
      if (store->op != NYIR_STORE_LOCAL || store->imm != def->imm)
        continue;
      nyr_scalar_class_t cls = store->scalar_class;
      if (cls == NYIR_SCALAR_UNKNOWN)
        cls = nyr_inline_value_class(f, types, store->a, depth + 1);
      if (cls == NYIR_SCALAR_UNKNOWN ||
          (joined != NYIR_SCALAR_UNKNOWN && joined != cls))
        return NYIR_SCALAR_UNKNOWN;
      joined = cls;
      saw_store = true;
    }
    if (saw_store)
      return joined;
  }
  if (def->scalar_class != NYIR_SCALAR_UNKNOWN)
    return def->scalar_class;
  if (def->op == NYIR_COPY)
    return nyr_inline_value_class(f, types, def->a, depth + 1);
  if (def->op == NYIR_PHI && def->phi_incoming_len) {
    nyr_scalar_class_t joined = NYIR_SCALAR_UNKNOWN;
    for (size_t i = 0; i < def->phi_incoming_len; ++i) {
      nyr_scalar_class_t cls = nyr_inline_value_class(
          f, types, def->phi_incoming[i].value, depth + 1);
      if (cls == NYIR_SCALAR_UNKNOWN ||
          (joined != NYIR_SCALAR_UNKNOWN && joined != cls))
        return NYIR_SCALAR_UNKNOWN;
      joined = cls;
    }
    return joined;
  }
  if (types && (size_t)value < types->value_count) {
    bool f32 = types->value_f32[value];
    bool f64 = types->value_f64[value];
    if (f32 != f64)
      return f64 ? NYIR_SCALAR_F64 : NYIR_SCALAR_F32;
  }
  return NYIR_SCALAR_UNKNOWN;
}

/*
 * Clone a callee with fresh SSA values and labels. PHIs are remapped from a
 * complete value map (including backedges), all RETs converge at a fresh
 * continuation PHI, and nested calls retain deep-copied owned metadata.
 */
static size_t nyr_inline_clone_body(nyr_func_t *caller, size_t call_idx,
                                     const nyr_func_t *callee,
                                     const int *arg_values, int arity,
                                     int call_dst, int *out_ret_dst) {
  if (!caller || !callee || callee->len == 0 || call_idx >= caller->len ||
      arity < 0 || arity > NYIR_CALL_MAX_ARGS)
    return 0;

  nyr_func_t clone = {0}, body = {0};
  nyr_type_map_t caller_types = {0};
  nyr_cfg_t ccfg = {0}, fcfg = {0};
  int *vmap = NULL, *ret_values = NULL;
  int64_t *labels = NULL, *ret_labels = NULL;
  bool materialized_formal[NYIR_CALL_MAX_ARGS] = {false};
  int formal_local[NYIR_CALL_MAX_ARGS];
  int caller_local_count = 0, callee_local_count = 0, local_base = 0;
  int materialized_count = 0;
  int next_value = caller->next_value;
  nyr_scalar_class_t call_result_class = caller->data[call_idx].scalar_class;
  int64_t next_label = 0, continuation = -1;
  size_t ret_count = 0, ret_at = 0, committed = 0;
  size_t call_block = 0;
  int64_t old_pred_label = -1;
  bool ok = false;

  if (!nyr_ir_func_clone(callee, &clone) ||
      !nyr_cfg_build_topology(&clone, &fcfg) || fcfg.block_count == 0 ||
      !nyr_cfg_build_topology(caller, &ccfg))
    goto done;
  if (!nyr_type_map_init(&caller_types, caller,
                          (size_t)nyr_inline_local_count(caller)))
    goto done;
  caller_local_count = nyr_inline_local_count(caller);
  callee_local_count = nyr_inline_local_count(&clone);
  for (int p = 0; p < NYIR_CALL_MAX_ARGS; ++p)
    formal_local[p] = -1;
  for (size_t i = 0; i < clone.len; ++i) {
    const nyr_inst_t *in = &clone.data[i];
    if ((in->op != NYIR_STORE_LOCAL && in->op != NYIR_ADDR_LOCAL) ||
        in->imm < 0 || in->imm >= arity)
      continue;
    if (!materialized_formal[in->imm]) {
      materialized_formal[in->imm] = true;
      ++materialized_count;
    }
  }
  int private_count =
      callee_local_count > arity ? callee_local_count - arity : 0;
  if (materialized_count > INT_MAX - private_count ||
      caller_local_count > INT_MAX - (materialized_count + private_count))
    goto done;
  local_base = caller_local_count;
  int next_formal_local = local_base;
  for (int p = 0; p < arity; ++p)
    if (materialized_formal[p])
      formal_local[p] = next_formal_local++;
  call_block = ccfg.inst_block[call_idx];
  old_pred_label = ccfg.block_label[call_block];

  /*
   * Inlining materializes the implicit invocation edge. An entry PHI has no
   * encoded value for that edge, so keep that unusual CFG shape out.
   */
  {
    size_t at = fcfg.block_start[0];
    if (at < fcfg.block_end[0] && clone.data[at].op == NYIR_LABEL)
      ++at;
    while (at < fcfg.block_end[0] && clone.data[at].op == NYIR_NOP)
      ++at;
    if (at < fcfg.block_end[0] && clone.data[at].op == NYIR_PHI)
      goto done;
  }

  /*
   * Do not let the cloned function physically fall through into caller code.
   */
  {
    size_t end = clone.len;
    while (end > 0 && (clone.data[end - 1].op == NYIR_NOP ||
                       clone.data[end - 1].op == NYIR_LABEL))
      --end;
    if (end == 0 || (clone.data[end - 1].op != NYIR_RET &&
                     clone.data[end - 1].op != NYIR_BR))
      goto done;
  }

  if (!nyr_inline_next_label(caller, &next_label) ||
      fcfg.block_count > (size_t)(INT64_MAX - next_label))
    goto done;
  labels = ny_malloc_array(fcfg.block_count, sizeof(*labels));
  if (!labels)
    goto done;
  for (size_t b = 0; b < fcfg.block_count; ++b)
    labels[b] = next_label++;
  if (next_label == INT64_MAX)
    goto done;
  continuation = next_label++;

  if (clone.next_value > 0) {
    vmap = ny_malloc_array((size_t)clone.next_value, sizeof(*vmap));
    if (!vmap)
      goto done;
    for (int v = 0; v < clone.next_value; ++v)
      vmap[v] = -1;
  }

  /*
   * Allocate every definition up front so PHI backedge values remap even when
   * their defining instruction appears later in linear order.
   */
  for (size_t i = 0; i < clone.len; ++i) {
    nyr_inst_t *in = &clone.data[i];
    if (in->dst < 0)
      continue;
    if (!vmap || in->dst >= clone.next_value)
      goto done;
    if (in->op == NYIR_LOAD_LOCAL && in->imm >= 0 && in->imm < arity &&
        !materialized_formal[in->imm]) {
      int actual = arg_values ? arg_values[in->imm] : -1;
      if (actual < 0 || actual >= caller->next_value)
        goto done;
      vmap[in->dst] = actual;
    } else {
      if (next_value == INT_MAX)
        goto done;
      vmap[in->dst] = next_value++;
    }
  }

  for (size_t i = 0; i < clone.len; ++i)
    if (clone.data[i].op == NYIR_RET)
      ++ret_count;
  if (call_dst >= 0 && ret_count == 0)
    goto done;
  if (ret_count) {
    ret_labels = ny_malloc_array(ret_count, sizeof(*ret_labels));
    if (!ret_labels)
      goto done;
    if (call_dst >= 0) {
      ret_values = ny_malloc_array(ret_count, sizeof(*ret_values));
      if (!ret_values)
        goto done;
    }
  }

  for (size_t b = 0; b < fcfg.block_count; ++b) {
    if (!nyr_inline_push(&body, (nyr_inst_t){.op = NYIR_LABEL,
                                               .dst = -1,
                                               .a = -1,
                                               .b = -1,
                                               .c = -1,
                                               .d = -1,
                                               .e = -1,
                                               .f = -1,
                                               .imm = labels[b]}))
      goto done;

    if (b == 0) {
      for (int p = 0; p < arity; ++p) {
        if (!materialized_formal[p])
          continue;
        int actual = arg_values ? arg_values[p] : -1;
        if (actual < 0 || actual >= caller->next_value || formal_local[p] < 0)
          goto done;
        nyr_scalar_class_t actual_class =
            nyr_inline_value_class(caller, &caller_types, actual, 0);
        if (ny_trace_enabled("NY_TRACE_ABI"))
          ny_trace_line("inline-local",
                        "formal=%d actual=v%d class=%d local=%d", p, actual,
                        (int)actual_class, formal_local[p]);
        if (!nyr_inline_push(
                &body, (nyr_inst_t){.op = NYIR_STORE_LOCAL,
                                     .dst = -1,
                                     .a = actual,
                                     .b = -1,
                                     .c = -1,
                                     .d = -1,
                                     .e = -1,
                                     .f = -1,
                                     .imm = formal_local[p],
                                     .scalar_class = actual_class,
                                     .effects = NYIR_EFFECT_WRITE_LOCAL}))
          goto done;
      }
    }

    for (size_t i = fcfg.block_start[b]; i < fcfg.block_end[b]; ++i) {
      nyr_inst_t *in = &clone.data[i];
      if (in->op == NYIR_NOP || in->op == NYIR_LABEL ||
          (in->op == NYIR_LOAD_LOCAL && in->imm >= 0 && in->imm < arity &&
           !materialized_formal[in->imm]))
        continue;

      if (in->op == NYIR_RET) {
        ret_labels[ret_at] = labels[b];
        if (call_dst >= 0) {
          int value = nyr_inline_value(vmap, clone.next_value, in->a);
          if (value < 0 || next_value == INT_MAX)
            goto done;
          ret_values[ret_at] = next_value;
          if (!nyr_inline_push(
                  &body, (nyr_inst_t){.op = NYIR_COPY,
                                       .dst = next_value++,
                                       .a = value,
                                       .b = -1,
                                       .c = -1,
                                       .d = -1,
                                       .e = -1,
                                       .f = -1,
                                       .scalar_class = call_result_class}))
            goto done;
        }
        ++ret_at;
        if (!nyr_inline_push(&body, (nyr_inst_t){.op = NYIR_BR,
                                                   .dst = -1,
                                                   .a = -1,
                                                   .b = -1,
                                                   .c = -1,
                                                   .d = -1,
                                                   .e = -1,
                                                   .f = -1,
                                                   .imm = continuation}))
          goto done;
        continue;
      }

      if (in->op == NYIR_BR || in->op == NYIR_BR_IF) {
        int target = nyr_inline_block_for_label(&fcfg, in->imm);
        if (target < 0)
          goto done;
        in->imm = labels[target];
        if (in->op == NYIR_BR_IF &&
            (in->a = nyr_inline_value(vmap, clone.next_value, in->a)) < 0)
          goto done;
      } else if (in->op == NYIR_PHI) {
        in->dst = nyr_inline_value(vmap, clone.next_value, in->dst);
        if (in->dst < 0)
          goto done;
        for (size_t p = 0; p < in->phi_incoming_len; ++p) {
          int pred = nyr_inline_block_for_label(
              &fcfg, in->phi_incoming[p].predecessor_label);
          int value = nyr_inline_value(vmap, clone.next_value,
                                        in->phi_incoming[p].value);
          if (pred < 0 || value < 0)
            goto done;
          in->phi_incoming[p].predecessor_label = labels[pred];
          in->phi_incoming[p].value = value;
        }
      } else {
        if ((in->op == NYIR_LOAD_LOCAL || in->op == NYIR_STORE_LOCAL ||
             in->op == NYIR_ADDR_LOCAL) &&
            in->imm >= 0) {
          if (in->imm < arity) {
            if (!materialized_formal[in->imm] || formal_local[in->imm] < 0)
              goto done;
            in->imm = formal_local[in->imm];
          } else {
            int64_t offset = in->imm - arity;
            int64_t mapped = (int64_t)local_base + materialized_count + offset;
            if (mapped < 0 || mapped >= INT_MAX)
              goto done;
            in->imm = mapped;
          }
          if (in->op == NYIR_STORE_LOCAL && ny_trace_enabled("NY_TRACE_ABI"))
            ny_trace_line("inline-local",
                          "callee-store a=v%d class=%d mapped-slot=%lld", in->a,
                          (int)in->scalar_class, (long long)in->imm);
        }
        if (in->dst >= 0 &&
            (in->dst = nyr_inline_value(vmap, clone.next_value, in->dst)) < 0)
          goto done;
        int *ops[] = {&in->a, &in->b, &in->c, &in->d, &in->e, &in->f};
        for (size_t p = 0; p < sizeof(ops) / sizeof(ops[0]); ++p)
          if (*ops[p] >= 0 && (*ops[p] = nyr_inline_value(
                                   vmap, clone.next_value, *ops[p])) < 0)
            goto done;
        for (size_t p = 0; p < in->extra_args_len; ++p)
          if ((in->extra_args[p] = nyr_inline_value(vmap, clone.next_value,
                                                     in->extra_args[p])) < 0)
            goto done;
      }

      if (!nyr_inline_take(&body, in))
        goto done;
    }
  }

  if (!nyr_inline_push(&body, (nyr_inst_t){.op = NYIR_LABEL,
                                             .dst = -1,
                                             .a = -1,
                                             .b = -1,
                                             .c = -1,
                                             .d = -1,
                                             .e = -1,
                                             .f = -1,
                                             .imm = continuation}))
    goto done;
  if (call_dst >= 0) {
    nyr_phi_incoming_t *incoming =
        ny_malloc_array(ret_count, sizeof(*incoming));
    if (!incoming)
      goto done;
    for (size_t r = 0; r < ret_count; ++r)
      incoming[r] = (nyr_phi_incoming_t){ret_labels[r], ret_values[r]};
    if (!nyr_inline_push(&body,
                          (nyr_inst_t){.op = NYIR_PHI,
                                        .dst = call_dst,
                                        .a = -1,
                                        .b = -1,
                                        .c = -1,
                                        .d = -1,
                                        .e = -1,
                                        .f = -1,
                                        .phi_incoming = incoming,
                                        .phi_incoming_len = ret_count,
                                        .scalar_class = call_result_class})) {
      free(incoming);
      goto done;
    }
  }

  if (body.len == 0 || body.len > SIZE_MAX - (caller->len - 1))
    goto done;
  size_t new_len = caller->len - 1 + body.len;
  if (new_len > caller->cap) {
    nyr_inst_t *data = ny_realloc_array(caller->data, new_len, sizeof(*data));
    if (!data)
      goto done;
    caller->data = data;
    caller->cap = new_len;
  }
  if (clone.owned_symbols_len > SIZE_MAX - caller->owned_symbols_len)
    goto done;
  size_t owned_need = caller->owned_symbols_len + clone.owned_symbols_len;
  if (owned_need > caller->owned_symbols_cap) {
    char **owned =
        ny_realloc_array(caller->owned_symbols, owned_need, sizeof(*owned));
    if (!owned)
      goto done;
    caller->owned_symbols = owned;
    caller->owned_symbols_cap = owned_need;
  }

  nyr_inst_discard(&caller->data[call_idx]);
  size_t tail = caller->len - call_idx - 1;
  memmove(&caller->data[call_idx + body.len], &caller->data[call_idx + 1],
          tail * sizeof(*caller->data));
  memcpy(&caller->data[call_idx], body.data, body.len * sizeof(*caller->data));
  caller->len = new_len;
  caller->next_value = next_value;
  for (size_t i = 0; i < clone.owned_symbols_len; ++i)
    caller->owned_symbols[caller->owned_symbols_len++] = clone.owned_symbols[i];

  /*
   * The original post-call tail is now in the continuation block. Update PHI
   * edges in each old successor to name that block instead of the split one.
   */
  size_t delta = body.len - 1;
  for (size_t e = ccfg.succ_offsets[call_block];
       e < ccfg.succ_offsets[call_block + 1]; ++e) {
    size_t at = ccfg.block_start[ccfg.succ_blocks[e]];
    if (at > call_idx)
      at += delta;
    if (at < caller->len && caller->data[at].op == NYIR_LABEL)
      ++at;
    while (at < caller->len && (caller->data[at].op == NYIR_NOP ||
                                caller->data[at].op == NYIR_PHI)) {
      if (caller->data[at].op == NYIR_PHI)
        for (size_t p = 0; p < caller->data[at].phi_incoming_len; ++p)
          if (caller->data[at].phi_incoming[p].predecessor_label ==
              old_pred_label)
            caller->data[at].phi_incoming[p].predecessor_label = continuation;
      ++at;
    }
  }

  committed = body.len;
  free(body.data);
  body.data = NULL;
  body.len = body.cap = 0;
  free(clone.owned_symbols);
  clone.owned_symbols = NULL;
  clone.owned_symbols_len = clone.owned_symbols_cap = 0;
  if (out_ret_dst)
    *out_ret_dst = call_dst;
  ok = true;

done:
  nyr_type_map_free(&caller_types);
  nyr_ir_func_free(&body);
  nyr_ir_func_free(&clone);
  nyr_cfg_free(&ccfg);
  nyr_cfg_free(&fcfg);
  free(vmap);
  free(labels);
  free(ret_values);
  free(ret_labels);
  return ok ? committed : 0;
}

static bool nyr_inline_value_is_i64_const(const nyr_func_t *f, int value,
                                           int64_t expected, unsigned depth) {
  if (!f || value < 0 || value >= f->next_value || depth > 32)
    return false;
  for (size_t i = f->len; i > 0; --i) {
    const nyr_inst_t *in = &f->data[i - 1];
    if (in->dst != value)
      continue;
    if (in->op == NYIR_CONST_I64)
      return in->imm == expected;
    if (in->op == NYIR_COPY)
      return nyr_inline_value_is_i64_const(f, in->a, expected, depth + 1);
    return false;
  }
  return false;
}

static bool nyr_inline_has_const_zero_division(const nyr_func_t *f) {
  if (!f)
    return false;
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if ((in->op == NYIR_DIV_I64 || in->op == NYIR_MOD_I64) &&
        nyr_inline_value_is_i64_const(f, in->b, 0, 0))
      return true;
  }
  return false;
}

/*
 * General inliner: inline profitable calls using the above machinery.
 */
bool nyr_inline_general(nyr_func_t *f) {
  if (!f || nyr_inline_callee_count == 0 || !f->has_symbol_calls)
    return true;

  nyr_inline_expansion_t *expansions = NULL;
  size_t expansion_capacity = 0;
  uint8_t *loop_depths = NULL;
  nyr_inline_chain_t chain = {0};

  /*
   * Every acyclic call chain can need one rescan per registered callee.
   */
  size_t max_rounds = nyr_inline_callee_count + 1u;
  /*
   * Cumulative growth budget: repeated splices must not expand a body without
   * bound. Limit total inserted instructions relative to the original body
   * size; once exhausted, stop later rescans too and leave remaining calls
   * out of line.
   */
  const size_t initial_len = f->len;
  const size_t splice_budget = initial_len * 16u + 2048u;
  size_t spliced_total = 0;
  bool changed_any = false;
  for (size_t iter = 0; iter < max_rounds; ++iter) {
    free(loop_depths);
    loop_depths = NULL;
    (void)nyr_inline_compute_loop_depths(f, &loop_depths);
    bool saw_symbol_call = false;
    if (ny_trace_enabled("NY_TRACE_LOWER") && ny_dbg_function_selected(NULL))
      if (ny_dbg_verbosity() >= 6)
        fprintf(stderr, "[inline_general] round %zu len=%zu\n", iter, f->len);
    bool changed = false;
    bool budget_exhausted = false;
    for (size_t i = 0; i < f->len; ++i) {
      nyr_inst_t *call = &f->data[i];
      if (call->op != NYIR_CALL || !call->symbol)
        continue;
      saw_symbol_call = true;

      const nyr_inline_callee_t *entry =
          nyr_find_inline_callee_entry(call->symbol);
      const nyr_func_t *callee = entry ? entry->func : NULL;
      if (!callee) {
        /*
         * Runtime builtins are never inline candidates; the reject is a
         * structural fact that repeats for every call site. Fold it into
         * the out-of-line aggregate instead of one line per site.
         */
        nyr_inline_ool_count++;
        nyr_inline_ool_last_reason =
            "no registered monomorphic NYIR callee body";
        continue;
      }
      if (!nyr_inline_summary_covered(call, callee)) {
        nyr_inline_trace(f, i, call->symbol, "reject",
                          "call-site effects exceed cloned body summary");
        continue;
      }
      /* Expansion ancestry is needed only for calls with a registered body
       * whose effects can be cloned. Runtime and rejected calls stay on the
       * allocation-free scan path. */
      if (!expansions) {
        expansions = ny_malloc_array(f->len, sizeof(*expansions));
        if (!expansions) {
          free(loop_depths);
          free(chain.data);
          return false;
        }
        for (size_t j = 0; j < f->len; ++j)
          expansions[j] = NYIR_INLINE_NO_EXPANSION;
        expansion_capacity = f->len;
      }
      const nyr_inline_expansion_t expansion = expansions[i];
      const size_t static_loop_depth =
          loop_depths ? loop_depths[i]
                      : nyr_inline_static_loop_depth_slow(f, i);
      if (!nyr_should_inline_general(f, call, callee, i, &chain, expansion,
                                      static_loop_depth, entry->name))
        continue;
      int args[NYIR_CALL_MAX_ARGS];
      int arity = 0;
      if (!nyr_call_args(call, f->next_value, args, NYIR_CALL_MAX_ARGS, &arity,
                          NULL, 0))
        continue;

      /*
       * A cloned body has at most one replacement label and two replacement
       * instructions per callee instruction, plus six materialized formals
       * and its continuation. Reserve context storage before mutating `f`.
       */
      nyr_inline_expansion_t child = NYIR_INLINE_NO_EXPANSION;
      if (!nyr_inline_chain_push(&chain, expansion, entry->name, &child)) {
        free(loop_depths);
        free(expansions);
        free(chain.data);
        return false;
      }
      if (callee->len > (SIZE_MAX - 8u) / 2u ||
          f->len - 1 > SIZE_MAX - (callee->len * 2u + 8u) ||
          !nyr_inline_expansion_reserve(&expansions, &expansion_capacity,
                                         f->len - 1 + callee->len * 2u + 8u)) {
        free(loop_depths);
        free(expansions);
        free(chain.data);
        return false;
      }

      const size_t old_len = f->len;
      const int call_dst = call->dst;
      const char *symbol = call->symbol;
      int ret_dst = -1;
      size_t inserted =
          nyr_inline_clone_body(f, i, callee, args, arity, call_dst, &ret_dst);
      if (inserted > 0) {
        nyr_inline_expansion_splice(expansions, old_len, i, inserted, child);
        free(loop_depths);
        loop_depths = NULL;
        (void)nyr_inline_compute_loop_depths(f, &loop_depths);
        changed = true;
        changed_any = true;
        if (nyr_inline_trace_enabled()) {
          if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
            ny_dbg_heuristic_event(
                "INLINE", "applied callee=%s pc=%zu inserted=%zu result=v%d",
                symbol ? symbol : "<unknown>", i, inserted, ret_dst);
          else
            ny_trace_line(
                "INLINE",
                "inline: applied callee=%s pc=%zu inserted=%zu result=v%d",
                symbol ? symbol : "<unknown>", i, inserted, ret_dst);
        }
        bool is_tiny_leaf =
            (callee->len <= 24 && !nyr_func_has_control_flow(callee));
        if (!is_tiny_leaf)
          spliced_total += inserted;
        if (spliced_total > splice_budget) {
          /*
           * Budget exhausted: keep the remaining calls as calls.  The body
           * is still valid IR; later cleanup passes see the same function.
           */
          if (ny_trace_enabled("NY_TRACE_LOWER") &&
              ny_dbg_function_selected(NULL))
            if (ny_dbg_verbosity() >= 6 || ny_trace_enabled("NY_TRACE_LOWER"))
              fprintf(stderr, "[inline_general] budget %zu exceeded, stop\n",
                      splice_budget);
          budget_exhausted = true;
          break;
        }
        i += inserted - 1;
      } else {
        nyr_inline_trace(f, i, symbol, "reject",
                          "callee clone/CFG splice legality check failed");
      }
    }
    if (budget_exhausted) {
      /* Calls copied from a just-spliced body may have been skipped by the
       * insertion cursor. Keep the hint conservative when stopping early. */
      f->has_symbol_calls = true;
      break;
    }
    if (!changed) {
      /* This complete, non-mutating pass observed every remaining direct
       * symbol call. Clear stale hints so later inliner stages can skip a
       * large function after its final eligible call has disappeared. */
      f->has_symbol_calls = saw_symbol_call;
      break;
    }
  }
  free(loop_depths);
  free(expansions);
  free(chain.data);
  if (changed_any && nyr_inline_has_const_zero_division(f)) {
    /*
     * Keep the inliner's output verifier-clean when actual arguments make a
     * callee guard constant. In particular, a guarded divide-by-zero in the
     * newly unreachable arm must be removed before the per-pass verifier sees
     * the splice; the ordinary pipeline cleanup runs too late for that gate.
     */
    if (!nyr_const_fold(f))
      return false;
    nyr_refresh_metadata(f);
    if (!nyr_cfg_simplify(f))
      return false;
    nyr_refresh_metadata(f);
    if (!nyr_dce(f))
      return false;
    nyr_refresh_metadata(f);
  }
  /*
   * Alias classes are function-local.  Cloned instructions carry the
   * callee's class numbers, which can collide with unrelated caller
   * allocations; rebuild the analysis after any successful splice before a
   * downstream memory pass can consume those facts.
   */
  return !changed_any || nyr_alias_analysis(f);
}

/*
 * Loop unrolling is implemented here with the other advanced NYIR transforms;
 * the pass keeps its local CFG and profile helpers next to its implementation.
 */
bool nyr_loop_unroll(nyr_func_t *f) {
  /*
   * Full unroll for tiny counted loops with a provably canonical shape:
   * - the compared value is a local slot stored exactly twice in the
   *   function: a constant-zero init before the guard and an add-one
   *   store inside the body;
   * - the guard is `load(slot) < const trip` with trip >= 2;
   * - the body is pure, lies entirely after the guard, and ends in a
   *   latch BR back to this loop's own header;
   * - the exit is an explicit BR whose target exists and is not part of
   *   the loop.
   * With init == 0 proven and trip >= 2, the entry guard is necessarily
   * taken, so replacing the backedge with straight-line copies preserves
   * the executed iteration count exactly.
   *
   * Partial unrolling was removed: its clone insertion wrote directly
   * into the live function tail without shifting it (clobbering the
   * latch and exit blocks), and its trip math silently assumed an entry
   * IV of zero without proof.
   */
  if (!f || f->len < 8)
    return true;
  for (size_t bi = 0; bi < f->len; ++bi) {
    if (f->data[bi].op != NYIR_BR_IF)
      continue;
    int cond = f->data[bi].a;
    int64_t body_lab = f->data[bi].imm;
    if (cond < 0)
      continue;
    /*
     * Find icmp.lt producing cond with const RHS trip.
     */
    int64_t trip = -1;
    int iv = -1;
    for (size_t j = 0; j < bi; ++j) {
      const nyr_inst_t *c = &f->data[j];
      if (c->op == NYIR_CMP_I64 && c->dst == cond && c->cmp == NYIR_CMP_LT) {
        iv = c->a;
        for (size_t k = 0; k < j; ++k)
          if (f->data[k].op == NYIR_CONST_I64 && f->data[k].dst == c->b) {
            trip = f->data[k].imm;
            break;
          }
        break;
      }
    }
    uint64_t loop_heat = ny_native_profile_loop_hot(bi);
    if (trip < 2 || iv < 0 || trip > 64) {
      if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
        ny_dbg_heuristic_event(
            "LOOP_UNROLL",
            "candidate reject pc=%zu trip=%lld iv=%d bounds=[2,64]", bi,
            (long long)trip, iv);
      continue;
    }

    /*
     * Body label region. The body must lie entirely after the guard so
     * clone insertion can never reorder the guard itself.
     */
    size_t body_start = 0, body_end = 0;
    bool have = false;
    for (size_t j = bi + 1; j < f->len; ++j) {
      if (f->data[j].op == NYIR_LABEL && f->data[j].imm == body_lab) {
        body_start = j + 1;
        for (size_t k = body_start; k < f->len; ++k) {
          if (f->data[k].op == NYIR_BR || f->data[k].op == NYIR_BR_IF ||
              f->data[k].op == NYIR_LABEL || f->data[k].op == NYIR_RET) {
            body_end = k;
            size_t body_limit = loop_heat >= 100 ? 128 : 96;
            have = (body_end - body_start) <= body_limit;
            break;
          }
        }
        break;
      }
    }
    if (!have) {
      if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
        ny_dbg_heuristic_event("LOOP_UNROLL",
                               "candidate reject pc=%zu trip=%lld "
                               "body=missing-or-over-budget budget=%u hot=%llu",
                               bi, (long long)trip,
                               loop_heat >= 100 ? 128u : 96u,
                               (unsigned long long)loop_heat);
      continue;
    }
    /*
     * Latch must branch back to this loop's own header. The guard's compare
     * and local loads are ordinary instructions between the label and the
     * conditional branch, so walk back through the block to its label.
     */
    int64_t header_lab = -1;
    size_t header_start = SIZE_MAX;
    for (size_t j = bi; j-- > 0;) {
      if (f->data[j].op == NYIR_LABEL) {
        header_lab = f->data[j].imm;
        header_start = j;
        break;
      }
      if (f->data[j].op == NYIR_BR || f->data[j].op == NYIR_BR_IF ||
          f->data[j].op == NYIR_RET)
        break;
    }
    if (header_lab < 0)
      continue;
    /*
     * This transform duplicates a linear body without remapping CFG edges.
     * PHIs outside this loop are irrelevant; PHIs in its header or body are
     * not. Keep the conservative legality boundary local to the candidate.
     */
    size_t loop_phi = SIZE_MAX;
    for (size_t j = header_start + 1; j < bi; ++j)
      if (f->data[j].op == NYIR_PHI) {
        loop_phi = j;
        break;
      }
    for (size_t j = body_start; loop_phi == SIZE_MAX && j < body_end; ++j)
      if (f->data[j].op == NYIR_PHI)
        loop_phi = j;
    if (loop_phi != SIZE_MAX) {
      if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
        ny_dbg_heuristic_event("LOOP_UNROLL",
                               "candidate reject pc=%zu: loop PHI at pc=%zu "
                               "requires CFG-edge-aware cloning",
                               bi, loop_phi);
      continue;
    }
    if (f->data[body_end].op != NYIR_BR ||
        f->data[body_end].imm != header_lab) {
      if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
        ny_dbg_heuristic_event("LOOP_UNROLL",
                               "candidate reject pc=%zu trip=%lld latch does "
                               "not branch to header=%lld",
                               bi, (long long)trip, (long long)header_lab);
      continue;
    }
    bool pure = true;
    for (size_t k = body_start; k < body_end; ++k) {
      nyr_op_t op = f->data[k].op;
      /*
       * Local loads/stores are safe across sequential copies: each copy keeps
       * the same local slot while the value map remaps its SSA operands.
       * Address-taken memory still needs alias-aware rematerialization.
       */
      if (op == NYIR_CALL || op == NYIR_PHI || op == NYIR_BR ||
          op == NYIR_BR_IF || op == NYIR_LABEL || op == NYIR_RET ||
          op == NYIR_ADDR_LOCAL || op == NYIR_ALLOCA) {
        pure = false;
        break;
      }
    }
    if (!pure) {
      if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
        ny_dbg_heuristic_event(
            "LOOP_UNROLL",
            "candidate reject pc=%zu trip=%lld body contains call, address "
            "escape, or unsupported control op",
            bi, (long long)trip);
      continue;
    }

    /*
     * Prove the induction shape: the compared value must be a local slot
     * stored exactly twice in the whole function — a constant-zero init
     * before the guard and an add-one store inside this body. Without
     * this proof the unrolled copy count can diverge from the original
     * trip count (loops entered with iv > 0 or IVs stepped by other
     * amounts).
     */
    int64_t iv_slot = -1;
    {
      const nyr_inst_t *iv_def = NULL;
      for (size_t j = 0; j < f->len; ++j)
        if (f->data[j].dst == iv) {
          iv_def = &f->data[j];
          break;
        }
      if (!iv_def || iv_def->op != NYIR_LOAD_LOCAL)
        continue;
      iv_slot = iv_def->imm;

/*
 * Trace a value through COPY chains to its producing instruction.
 */
#define UNROLL_TRACE_TO(v, out)                                                \
  do {                                                                         \
    int _v = (v);                                                              \
    (out) = NULL;                                                              \
    for (int _depth = 0; _depth < 8 && _v >= 0; ++_depth) {                    \
      const nyr_inst_t *_def = NULL;                                          \
      for (size_t _j = 0; _j < f->len; ++_j)                                   \
        if (f->data[_j].dst == _v) {                                           \
          _def = &f->data[_j];                                                 \
          break;                                                               \
        }                                                                      \
      if (!_def)                                                               \
        break;                                                                 \
      if (_def->op == NYIR_COPY) {                                             \
        _v = _def->a;                                                          \
        continue;                                                              \
      }                                                                        \
      (out) = _def;                                                            \
      break;                                                                   \
    }                                                                          \
  } while (0)

      const nyr_inst_t *init_def = NULL, *step_def = NULL;
      size_t init_at = 0;
      bool other_store = false;
      for (size_t j = 0; j < f->len; ++j) {
        const nyr_inst_t *in = &f->data[j];
        if (in->op != NYIR_STORE_LOCAL || in->imm != iv_slot)
          continue;
        if (j > bi && j < body_start) {
          /*
           * Store between guard and body label: not a shape we prove.
           */
          other_store = true;
          break;
        }
        if (j >= body_start && j < body_end) {
          if (step_def) {
            other_store = true;
            break;
          }
          UNROLL_TRACE_TO(in->a, step_def);
          continue;
        }
        if (init_def) {
          other_store = true;
          break;
        }
        UNROLL_TRACE_TO(in->a, init_def);
        init_at = j;
      }
      if (other_store || !init_def || !step_def ||
          init_def->op != NYIR_CONST_I64 || init_def->imm != 0 ||
          init_at > bi || step_def->op != NYIR_ADD_I64) {
        if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
          ny_dbg_heuristic_event(
              "LOOP_UNROLL",
              "candidate reject pc=%zu induction proof failed: init-zero=%s "
              "step-add=%s single-store=%s",
              bi,
              init_def && init_def->op == NYIR_CONST_I64 && init_def->imm == 0
                  ? "yes"
                  : "no",
              step_def && step_def->op == NYIR_ADD_I64 ? "yes" : "no",
              other_store ? "no" : "yes");
        continue;
      }
      const nyr_inst_t *step_lhs = NULL, *step_rhs = NULL;
      for (size_t j = 0; j < f->len && !step_lhs; ++j)
        if (f->data[j].dst == step_def->a)
          step_lhs = &f->data[j];
      for (size_t j = 0; j < f->len && !step_rhs; ++j)
        if (f->data[j].dst == step_def->b)
          step_rhs = &f->data[j];
      bool reloads_iv = step_lhs && step_lhs->op == NYIR_LOAD_LOCAL &&
                        step_lhs->imm == iv_slot;
      const nyr_inst_t *one = reloads_iv ? step_rhs : step_lhs;
      if (!reloads_iv || !one || one->op != NYIR_CONST_I64 || one->imm != 1) {
        if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
          ny_dbg_heuristic_event(
              "LOOP_UNROLL",
              "candidate reject pc=%zu induction proof failed: reload-iv=%s "
              "step-one=%s",
              bi, reloads_iv ? "yes" : "no",
              one && one->op == NYIR_CONST_I64 && one->imm == 1 ? "yes" : "no");
        continue;
      }
#undef UNROLL_TRACE_TO
    }

    size_t body_len = body_end - body_start;
    /*
     * Keep diagnostic and production compilation bounded.  Large unroll
     * expansions amplify SSA remapping and CFG edge risk without a measured
     * benefit; leave those loops in their canonical form.
     */
    if (body_len == 0 || trip > 64 || body_len > SIZE_MAX / (size_t)trip ||
        body_len * (size_t)trip > 4096) {
      if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
        ny_dbg_heuristic_event("LOOP_UNROLL",
                               "candidate reject pc=%zu trip=%lld "
                               "body-inst=%zu expansion=%zu cap=4096",
                               bi, (long long)trip, body_len,
                               body_len <= SIZE_MAX / (size_t)trip
                                   ? body_len * (size_t)trip
                                   : SIZE_MAX);
      continue;
    }
    {
      /*
       * Full unroll. The exit target must be an explicit BR after the
       * guard whose label exists and lies outside the loop.
       */
      size_t extra = (size_t)(trip - 1) * (body_end - body_start);
      if (extra == 0)
        continue;
      if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
        ny_dbg_heuristic_event(
            "LOOP_UNROLL",
            "candidate accept pc=%zu trip=%lld body-inst=%zu added-inst=%zu "
            "total-expansion=%zu hot=%llu",
            bi, (long long)trip, body_len, extra, body_len * (size_t)trip,
            (unsigned long long)loop_heat);
      if (f->len + extra > f->cap) {
        size_t nc = f->cap ? f->cap * 2 : 64;
        while (nc < f->len + extra)
          nc *= 2;
        nyr_inst_t *nd = ny_realloc_array(f->data, nc, sizeof(*nd));
        if (!nd)
          return false;
        f->data = nd;
        f->cap = nc;
      }
      size_t insert_at = body_end;
      memmove(&f->data[insert_at + extra], &f->data[insert_at],
              (f->len - insert_at) * sizeof(nyr_inst_t));
      f->len += extra;
      for (int t = 1; t < (int)trip; ++t) {
        size_t base = insert_at + (size_t)(t - 1) * (body_end - body_start);
        int *vmap = NULL;
        size_t vmap_n =
            (size_t)(f->next_value + (int)(body_end - body_start) + 8);
        if (vmap_n < 8)
          vmap_n = 8;
        vmap = ny_malloc_array(vmap_n, sizeof(int));
        if (!vmap)
          return false;
        for (size_t m = 0; m < vmap_n; ++m)
          vmap[m] = -1;
        for (size_t k = 0; k < (size_t)(body_end - body_start); ++k) {
          nyr_inst_t in = f->data[body_start + k];
          int old_dst = in.dst;
          if (in.dst >= 0) {
            int nd = f->next_value++;
            if ((size_t)old_dst >= vmap_n) {
              size_t nn = (size_t)old_dst + 8;
              int *nv = ny_realloc_array(vmap, nn, sizeof(int));
              if (!nv) {
                free(vmap);
                return false;
              }
              for (size_t m = vmap_n; m < nn; ++m)
                nv[m] = -1;
              vmap = nv;
              vmap_n = nn;
            }
            vmap[old_dst] = nd;
            in.dst = nd;
          }
          if (in.a >= 0 && (size_t)in.a < vmap_n && vmap[in.a] >= 0)
            in.a = vmap[in.a];
          if (in.b >= 0 && (size_t)in.b < vmap_n && vmap[in.b] >= 0)
            in.b = vmap[in.b];
          if (in.c >= 0 && (size_t)in.c < vmap_n && vmap[in.c] >= 0)
            in.c = vmap[in.c];
          if (in.d >= 0 && (size_t)in.d < vmap_n && vmap[in.d] >= 0)
            in.d = vmap[in.d];
          f->data[base + k] = in;
        }
        free(vmap);
      }
      /*
       * After unrolling copies, force exit. The exit must be an explicit
       * BR immediately reachable from the guard whose target exists and
       * lies outside this loop; a LABEL fallback could pick the body
       * label itself and leave an infinite loop behind.
       */
      int64_t exit_lab = -1;
      for (size_t j = bi + 1; j < f->len; ++j) {
        if (f->data[j].op == NYIR_NOP)
          continue;
        if (f->data[j].op == NYIR_BR && f->data[j].imm != body_lab &&
            f->data[j].imm != header_lab)
          exit_lab = f->data[j].imm;
        break;
      }
      if (exit_lab >= 0) {
        bool exit_exists = false;
        for (size_t j = 0; j < f->len; ++j)
          if (f->data[j].op == NYIR_LABEL && f->data[j].imm == exit_lab) {
            exit_exists = true;
            break;
          }
        if (!exit_exists)
          exit_lab = -1;
      }
      if (exit_lab >= 0) {
        size_t latch = body_end + extra;
        if (latch < f->len && f->data[latch].op == NYIR_BR)
          f->data[latch].imm = exit_lab;
      }
      return true;
    }
  }
  return true;
}

bool nyr_algebraic_combine(nyr_func_t *f) {
  if (!f || f->next_value <= 0)
    return true;
  bool *known = calloc((size_t)f->next_value, sizeof(bool));
  int64_t *value = calloc((size_t)f->next_value, sizeof(int64_t));
  if (!known || !value) {
    free(known);
    free(value);
    return false;
  }
  for (size_t i = 0; i < f->len; ++i) {
    nyr_inst_t *in = &f->data[i];
    if (in->op == NYIR_CONST_I64 && in->dst >= 0 &&
        (size_t)in->dst < (size_t)f->next_value) {
      known[in->dst] = true;
      value[in->dst] = in->imm;
    }
    /*
     * (x + c1) + c2 → x + (c1+c2)
     */
    if (in->op == NYIR_ADD_I64 && in->a >= 0 && in->b >= 0 && known[in->b]) {
      for (size_t j = 0; j < i; ++j) {
        nyr_inst_t *p = &f->data[j];
        if (p->op == NYIR_ADD_I64 && p->dst == in->a && p->b >= 0 &&
            known[p->b] && p->a >= 0) {
          int64_t sum = value[p->b] + value[in->b];
          int cdst = f->next_value++;
          if (f->len + 1 > f->cap) {
            size_t nc = f->cap ? f->cap * 2 : 64;
            nyr_inst_t *nd = ny_realloc_array(f->data, nc, sizeof(*nd));
            if (!nd) {
              free(known);
              free(value);
              return false;
            }
            f->data = nd;
            f->cap = nc;
            in = &f->data[i];
            p = &f->data[j];
          }
          memmove(&f->data[i + 1], &f->data[i],
                  (f->len - i) * sizeof(nyr_inst_t));
          f->len++;
          f->data[i] = (nyr_inst_t){
              .op = NYIR_CONST_I64,
              .dst = cdst,
              .imm = sum,
              .range = {
                  .has_min = true, .has_max = true, .min = sum, .max = sum}};
          f->data[i + 1] = (nyr_inst_t){
              .op = NYIR_ADD_I64, .dst = in->dst, .a = p->a, .b = cdst};
          bool *new_known =
              ny_realloc_array(known, (size_t)f->next_value, sizeof(bool));
          int64_t *new_value =
              ny_realloc_array(value, (size_t)f->next_value, sizeof(int64_t));
          if (!new_known || !new_value) {
            free(new_known ? new_known : known);
            free(new_value ? new_value : value);
            return false;
          }
          known = new_known;
          value = new_value;
          known[cdst] = true;
          value[cdst] = sum;
          i++;
          break;
        }
      }
    }
    /*
     * (x & m1) & m2 → x & (m1&m2)
     */
    if (in->op == NYIR_AND_I64 && in->a >= 0 && in->b >= 0 && known[in->b]) {
      for (size_t j = 0; j < i; ++j) {
        nyr_inst_t *p = &f->data[j];
        if (p->op == NYIR_AND_I64 && p->dst == in->a && p->b >= 0 &&
            known[p->b] && p->a >= 0) {
          int64_t m = value[p->b] & value[in->b];
          int cdst = f->next_value++;
          if (f->len + 1 > f->cap) {
            size_t nc = f->cap ? f->cap * 2 : 64;
            nyr_inst_t *nd = ny_realloc_array(f->data, nc, sizeof(*nd));
            if (!nd) {
              free(known);
              free(value);
              return false;
            }
            f->data = nd;
            f->cap = nc;
            in = &f->data[i];
            p = &f->data[j];
          }
          memmove(&f->data[i + 1], &f->data[i],
                  (f->len - i) * sizeof(nyr_inst_t));
          f->len++;
          f->data[i] = (nyr_inst_t){
              .op = NYIR_CONST_I64,
              .dst = cdst,
              .imm = m,
              .range = {.has_min = true, .has_max = true, .min = m, .max = m}};
          f->data[i + 1] = (nyr_inst_t){
              .op = NYIR_AND_I64, .dst = in->dst, .a = p->a, .b = cdst};
          bool *new_known =
              ny_realloc_array(known, (size_t)f->next_value, sizeof(bool));
          int64_t *new_value =
              ny_realloc_array(value, (size_t)f->next_value, sizeof(int64_t));
          if (!new_known || !new_value) {
            free(new_known ? new_known : known);
            free(new_value ? new_value : value);
            return false;
          }
          known = new_known;
          value = new_value;
          known[cdst] = true;
          value[cdst] = m;
          i++;
          break;
        }
      }
    }
  }
  free(known);
  free(value);
  return true;
}

bool nyr_double_neg(nyr_func_t *f) {
  if (!f)
    return true;
  /*
   * ~(~x) / -(-x) via xor -1 twice or sub 0 patterns: x ^ -1 twice → copy x
   */
  for (size_t i = 0; i < f->len; ++i) {
    nyr_inst_t *in = &f->data[i];
    if (in->op != NYIR_XOR_I64 || in->a < 0 || in->b < 0)
      continue;
    /*
     * find if b is const -1 and a is also xor with -1
     */
    bool b_neg1 = false;
    for (size_t j = 0; j < i; ++j)
      if (f->data[j].op == NYIR_CONST_I64 && f->data[j].dst == in->b &&
          f->data[j].imm == -1)
        b_neg1 = true;
    if (!b_neg1)
      continue;
    for (size_t j = 0; j < i; ++j) {
      nyr_inst_t *p = &f->data[j];
      if (p->op == NYIR_XOR_I64 && p->dst == in->a && p->a >= 0 && p->b >= 0) {
        bool pb = false;
        for (size_t k = 0; k < j; ++k)
          if (f->data[k].op == NYIR_CONST_I64 && f->data[k].dst == p->b &&
              f->data[k].imm == -1)
            pb = true;
        if (pb) {
          nir_make_copy(in, p->a);
          break;
        }
      }
    }
  }
  /*
   * sub 0, x → neg; sub 0, (sub 0, x) → copy x
   */
  for (size_t i = 0; i < f->len; ++i) {
    nyr_inst_t *in = &f->data[i];
    if (in->op != NYIR_SUB_I64 || in->a < 0 || in->b < 0)
      continue;
    bool a0 = false;
    for (size_t j = 0; j < i; ++j)
      if (f->data[j].op == NYIR_CONST_I64 && f->data[j].dst == in->a &&
          f->data[j].imm == 0)
        a0 = true;
    if (!a0)
      continue;
    for (size_t j = 0; j < i; ++j) {
      nyr_inst_t *p = &f->data[j];
      if (p->op == NYIR_SUB_I64 && p->dst == in->b && p->a >= 0 && p->b >= 0) {
        bool pa0 = false;
        for (size_t k = 0; k < j; ++k)
          if (f->data[k].op == NYIR_CONST_I64 && f->data[k].dst == p->a &&
              f->data[k].imm == 0)
            pa0 = true;
        if (pa0) {
          nir_make_copy(in, p->b);
          break;
        }
      }
    }
  }
  return true;
}

bool nyr_reassoc_add(nyr_func_t *f) {
  /*
   * (a + b) + c with c const, b const already handled in algebraic; here
   * reassoc (a + k1) + (b + k2) is out of scope. Reassoc a + (b + k) →
   * (a + b) + k when k const for better fold.
   */
  if (!f || f->next_value <= 0)
    return true;
  bool *known = calloc((size_t)f->next_value, sizeof(bool));
  int64_t *value = calloc((size_t)f->next_value, sizeof(int64_t));
  int *def_idx = malloc((size_t)f->next_value * sizeof(*def_idx));
  if (!known || !value || !def_idx) {
    free(known);
    free(value);
    free(def_idx);
    return false;
  }
  memset(def_idx, -1, (size_t)f->next_value * sizeof(*def_idx));

  for (size_t i = 0; i < f->len; ++i) {
    nyr_inst_t *in = &f->data[i];
    if (in->dst >= 0 && (size_t)in->dst < (size_t)f->next_value)
      def_idx[in->dst] = (int)i;
    if (in->op == NYIR_CONST_I64 && in->dst >= 0 &&
        (size_t)in->dst < (size_t)f->next_value) {
      known[in->dst] = true;
      value[in->dst] = in->imm;
    }
    if (in->op != NYIR_ADD_I64 || in->a < 0 || in->b < 0)
      continue;
    /*
     * a + (b + k) where right is add with const
     */
    int j = (in->b < f->next_value) ? def_idx[in->b] : -1;
    if (j >= 0 && (size_t)j < i) {
      nyr_inst_t *r = &f->data[j];
      if (r->op == NYIR_ADD_I64 && r->dst == in->b && r->a >= 0 && r->b >= 0 &&
          known[r->b]) {
        int tdst = f->next_value++;
        if (f->len + 1 > f->cap) {
          size_t nc = f->cap ? f->cap * 2 : 64;
          nyr_inst_t *nd = ny_realloc_array(f->data, nc, sizeof(*nd));
          if (!nd) {
            free(known);
            free(value);
            free(def_idx);
            return false;
          }
          f->data = nd;
          f->cap = nc;
          in = &f->data[i];
          r = &f->data[j];
        }
        memmove(&f->data[i + 1], &f->data[i],
                (f->len - i) * sizeof(nyr_inst_t));
        f->len++;
        int adst = in->dst;
        int aa = in->a;
        f->data[i] =
            (nyr_inst_t){.op = NYIR_ADD_I64, .dst = tdst, .a = aa, .b = r->a};
        f->data[i + 1] = (nyr_inst_t){
            .op = NYIR_ADD_I64, .dst = adst, .a = tdst, .b = r->b};
        i++;
      }
    }
  }
  free(known);
  free(value);
  free(def_idx);
  return true;
}

bool nyr_rewrite_fuel(nyr_func_t *f) {
  /*
   * Run the local rewrite group to a structural fixed point.  Instruction
   * count is not a sufficient progress test: reassociation and algebraic
   * canonicalization can change operands/opcodes without changing f->len,
   * while two individually-correct rules can also oscillate between equal-
   * sized forms.  Fingerprints let us stop on both no-progress and cycles.
   *
   * The bounded history remains a hard safety/resource limit for malformed
   * or unexpectedly adversarial IR, but normal termination is driven by
   * useful structural progress rather than a fixed number of blind repeats.
   */
  if (!f)
    return true;
  enum { NYIR_REWRITE_HISTORY = 16 };
  uint64_t seen[NYIR_REWRITE_HISTORY] = {0};
  size_t seen_len = 0;
  seen[seen_len++] = nyr_debug_fingerprint(f);

  for (;;) {
    if (!nyr_apply_rules(f) || !nyr_double_neg(f) ||
        !nyr_algebraic_combine(f) || !nyr_reassoc_add(f))
      return false;

    uint64_t after = nyr_debug_fingerprint(f);
    if (after == seen[seen_len - 1])
      break;

    bool cycle = false;
    for (size_t i = 0; i < seen_len; ++i) {
      if (seen[i] == after) {
        cycle = true;
        break;
      }
    }
    if (cycle)
      break;
    if (seen_len == NYIR_REWRITE_HISTORY)
      break;
    seen[seen_len++] = after;
  }
  return true;
}

static bool nyr_tbuf_const_before(const nyr_func_t *f, int value,
                                   size_t before, int64_t *out) {
  if (!f || value < 0 || !out)
    return false;
  for (size_t i = 0; i < before && i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->dst == value) {
      if (in->op == NYIR_CONST_I64) {
        *out = in->imm;
        return true;
      }
      if (in->op == NYIR_COPY)
        return nyr_tbuf_const_before(f, in->a, i, out);
      if (in->op == NYIR_MUL_I64) {
        int64_t ca = 0, cb = 0;
        if (nyr_tbuf_const_before(f, in->a, i, &ca) &&
            nyr_tbuf_const_before(f, in->b, i, &cb)) {
          *out = ca * cb;
          return true;
        }
      }
      if (in->op == NYIR_ADD_I64) {
        int64_t ca = 0, cb = 0;
        if (nyr_tbuf_const_before(f, in->a, i, &ca) &&
            nyr_tbuf_const_before(f, in->b, i, &cb)) {
          *out = ca + cb;
          return true;
        }
      }
    }
  }
  return false;
}

static bool nyr_tbuf_uses_exact(const nyr_use_def_t *uses, int value,
                                 size_t first, size_t second, bool two) {
  if (!uses || value < 0 || (size_t)value >= uses->value_count)
    return false;
  size_t begin = uses->offsets[value];
  size_t end = uses->offsets[value + 1];
  if (end - begin != (two ? 2u : 1u))
    return false;
  bool have_first = false, have_second = !two;
  for (size_t i = begin; i < end; ++i) {
    if (uses->users[i] == first)
      have_first = true;
    else if (two && uses->users[i] == second)
      have_second = true;
    else
      return false;
  }
  return have_first && have_second;
}

static bool nyr_tbuf_linear_between(const nyr_func_t *f, size_t begin,
                                     size_t end) {
  if (!f || begin > end || end > f->len)
    return false;
  for (size_t i = begin; i < end; ++i) {
    nyr_op_t op = f->data[i].op;
    if (op == NYIR_CALL || op == NYIR_LABEL || op == NYIR_BR ||
        op == NYIR_BR_IF || op == NYIR_PHI || op == NYIR_RET)
      return false;
  }
  return true;
}
/*
 * Scalarize a non-escaping zero-length tbuf append chain followed only by a
 * length query.  Every intermediate pointer must be consumed exactly once by
 * the next append (and the final pointer exactly once by len), so removing the
 * private allocation/mutations cannot change observable state.  Values passed
 * to append remain evaluated normally; only the dead managed-buffer traffic is
 * removed.
 */
bool nyr_tbuf_scalar_len(nyr_func_t *f) {
  if (!f || f->len == 0)
    return true;
  nyr_use_def_t uses = {0};
  if (!nyr_build_use_def(f, &uses))
    return false;

  for (size_t ni = 0; ni < f->len; ++ni) {
    nyr_inst_t *alloc = &f->data[ni];
    if (alloc->op != NYIR_CALL || !alloc->symbol ||
        strcmp(alloc->symbol, "rt_tbuf_new_raw") != 0 || alloc->dst < 0)
      continue;
    int64_t count = 0, elem_size = 0;
    if (alloc->a < 0 || alloc->b < 0 ||
        !nyr_tbuf_const_before(f, alloc->a, ni, &count) ||
        !nyr_tbuf_const_before(f, alloc->b, ni, &elem_size) || count != 0 ||
        elem_size <= 0)
      continue;

    /*
     * new(0, width) followed only by len is exactly zero even when the
     * allocation itself fails (rt_tbuf_len_raw(0) == 0).  Unlike an
     * append-chain fold, this therefore preserves the runtime's null/OOM
     * behavior while eliminating both the dead allocation and helper call.
     */
    if (count == 0 && (size_t)alloc->dst < uses.value_count) {
      size_t begin = uses.offsets[alloc->dst];
      size_t end = uses.offsets[alloc->dst + 1];
      if (end - begin == 1u) {
        size_t user_idx = uses.users[begin];
        if (user_idx < f->len) {
          nyr_inst_t *user = &f->data[user_idx];
          if (user->op == NYIR_CALL && user->symbol &&
              strcmp(user->symbol, "rt_tbuf_len_raw") == 0 &&
              user->a == alloc->dst && user->dst >= 0) {
            int dst = user->dst;
            nyr_inst_discard(user);
            f->data[user_idx] = (nyr_inst_t){.op = NYIR_CONST_I64,
                                              .dst = dst,
                                              .a = -1,
                                              .b = -1,
                                              .c = -1,
                                              .d = -1,
                                              .e = -1,
                                              .f = -1,
                                              .imm = 0};
            nyr_inst_discard(alloc);
            nyr_use_def_free(&uses);
            return true;
          }
        }
      }
    }

    size_t cap = 8, append_len = 0;
    size_t *append_idx = malloc(cap * sizeof(*append_idx));
    int *chain_values = malloc((cap + 1u) * sizeof(*chain_values));
    if (!append_idx || !chain_values) {
      free(append_idx);
      free(chain_values);
      nyr_use_def_free(&uses);
      return false;
    }
    chain_values[0] = alloc->dst;
    int current = alloc->dst;
    size_t last = ni;
    bool committed = false;

    for (size_t i = ni + 1; i < f->len; ++i) {
      nyr_inst_t *in = &f->data[i];
      if (in->op == NYIR_LABEL || in->op == NYIR_BR || in->op == NYIR_BR_IF ||
          in->op == NYIR_PHI || in->op == NYIR_RET)
        break;
      if (in->op != NYIR_CALL)
        continue;
      if (!in->symbol)
        break;

      bool is_append = strcmp(in->symbol, "rt_tbuf_append_raw") == 0 ||
                       strcmp(in->symbol, "rt_tbuf_append_i64_raw") == 0;
      if (is_append && in->a == current && in->dst >= 0 && in->dst != current &&
          nyr_tbuf_linear_between(f, last + 1u, i)) {
        if (append_len == cap) {
          size_t new_cap = cap * 2u;
          size_t *new_idx = realloc(append_idx, new_cap * sizeof(*new_idx));
          if (!new_idx) {
            free(append_idx);
            free(chain_values);
            nyr_use_def_free(&uses);
            return false;
          }
          append_idx = new_idx;
          int *new_values =
              realloc(chain_values, (new_cap + 1u) * sizeof(*new_values));
          if (!new_values) {
            free(append_idx);
            free(chain_values);
            nyr_use_def_free(&uses);
            return false;
          }
          chain_values = new_values;
          cap = new_cap;
        }
        append_idx[append_len] = i;
        chain_values[append_len + 1u] = in->dst;
        ++append_len;
        current = in->dst;
        last = i;
        continue;
      }

      if (append_len > 0 && strcmp(in->symbol, "rt_tbuf_len_raw") == 0 &&
          in->a == current && in->dst >= 0 &&
          nyr_tbuf_linear_between(f, last + 1u, i)) {
        bool safe = true;
        for (size_t a = 0; a < append_len; ++a) {
          size_t user = append_idx[a];
          if (!nyr_tbuf_uses_exact(&uses, chain_values[a], user, user,
                                    false)) {
            safe = false;
            break;
          }
        }
        if (safe &&
            !nyr_tbuf_uses_exact(&uses, chain_values[append_len], i, i, false))
          safe = false;
        if (!safe)
          break;

        int dst = in->dst;
        nyr_inst_discard(in);
        f->data[i] = (nyr_inst_t){.op = NYIR_CONST_I64,
                                   .dst = dst,
                                   .a = -1,
                                   .b = -1,
                                   .c = -1,
                                   .d = -1,
                                   .e = -1,
                                   .f = -1,
                                   .imm = (int64_t)append_len};
        for (size_t a = 0; a < append_len; ++a)
          nyr_inst_discard(&f->data[append_idx[a]]);
        nyr_inst_discard(alloc);
        committed = true;
        break;
      }

      /*
       * Any other call can observe/perturb state, so this chain is not local.
       */
      break;
    }
    free(append_idx);
    free(chain_values);
    if (committed) {
      nyr_use_def_free(&uses);
      return true;
    }
  }
  nyr_use_def_free(&uses);
  return true;
}

static bool nyr_tbuf_eval_const_offset(const nyr_func_t *f, int v, int base,
                                        int64_t *out_off, int depth) {
  if (depth > 16)
    return false;
  if (base >= 0 && v == base) {
    *out_off = 0;
    return true;
  }
  if (v < 0 || v >= f->next_value)
    return false;
  for (size_t i = 0; i < f->len; ++i) {
    if (f->data[i].dst == v) {
      const nyr_inst_t *in = &f->data[i];
      if (in->op == NYIR_COPY && in->a >= 0)
        return nyr_tbuf_eval_const_offset(f, in->a, base, out_off, depth + 1);
      if (in->op == NYIR_ADD_I64) {
        int64_t off1 = 0, off2 = 0;
        if (nyr_tbuf_eval_const_offset(f, in->a, base, &off1, depth + 1) &&
            nyr_tbuf_eval_const_offset(f, in->b, -1, &off2, depth + 1)) {
          *out_off = off1 + off2;
          return true;
        }
        if (nyr_tbuf_eval_const_offset(f, in->b, base, &off1, depth + 1) &&
            nyr_tbuf_eval_const_offset(f, in->a, -1, &off2, depth + 1)) {
          *out_off = off1 + off2;
          return true;
        }
      }
      if (in->op == NYIR_CONST_I64 && base == -1) {
        *out_off = in->imm;
        return true;
      }
      if (in->op == NYIR_MUL_I64 && base == -1) {
        int64_t c1 = 0, c2 = 0;
        if (nyr_tbuf_eval_const_offset(f, in->a, -1, &c1, depth + 1) &&
            nyr_tbuf_eval_const_offset(f, in->b, -1, &c2, depth + 1)) {
          *out_off = c1 * c2;
          return true;
        }
      }
      if (in->op == NYIR_SHL_I64 && base == -1) {
        int64_t c1 = 0, c2 = 0;
        if (nyr_tbuf_eval_const_offset(f, in->a, -1, &c1, depth + 1) &&
            nyr_tbuf_eval_const_offset(f, in->b, -1, &c2, depth + 1) &&
            c2 >= 0 && c2 < 64) {
          *out_off = c1 << c2;
          return true;
        }
      }
      break;
    }
  }
  return false;
}

/*
 * Evaluate the byte length used by a bounds check when it comes from the
 * queried length of this exact private buffer.  This is deliberately narrower
 * than general constant folding: only the allocation's own length query and
 * checked scalar arithmetic are accepted.
 */
static bool nyr_tbuf_eval_private_length(const nyr_func_t *f, int value,
                                          const nyr_inst_t *alloc,
                                          int alloc_dst, int64_t *out,
                                          unsigned depth) {
  if (!f || !alloc || !out || value < 0 || value >= f->next_value || depth > 12)
    return false;
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->dst != value)
      continue;
    if (in->op == NYIR_CONST_I64) {
      *out = in->imm;
      return true;
    }
    if (in->op == NYIR_COPY)
      return nyr_tbuf_eval_private_length(f, in->a, alloc, alloc_dst, out,
                                           depth + 1);
    if (in->op == NYIR_CALL && in->symbol &&
        strcmp(in->symbol, "rt_tbuf_len_raw") == 0 &&
        nyr_tbuf_eval_const_offset(f, in->a, alloc_dst, out, 0) && *out == 0)
      return nyr_tbuf_const_before(f, alloc->a, i, out);
    if (in->op == NYIR_ADD_I64 || in->op == NYIR_MUL_I64 ||
        in->op == NYIR_SHL_I64) {
      int64_t a = 0, b = 0, result = 0;
      if (!nyr_tbuf_eval_private_length(f, in->a, alloc, alloc_dst, &a,
                                         depth + 1) ||
          !nyr_tbuf_eval_private_length(f, in->b, alloc, alloc_dst, &b,
                                         depth + 1))
        return false;
      bool overflow = false;
      if (in->op == NYIR_ADD_I64)
        overflow = __builtin_add_overflow(a, b, &result);
      else if (in->op == NYIR_MUL_I64)
        overflow = __builtin_mul_overflow(a, b, &result);
      else if (b < 0 || b >= 63 || a < 0 || a > (INT64_MAX >> (unsigned)b))
        overflow = true;
      else
        result = a << (unsigned)b;
      if (overflow)
        return false;
      *out = result;
      return true;
    }
    return false;
  }
  return false;
}

/*
 * Promote small, fixed-size typed-buffer workspaces to fixed stack storage.
 *
 * `rt_tbuf_new_typed_raw` is used by the compiler for raw numeric buffers.
 * When the returned payload pointer never escapes and is used only as the base
 * of direct loads/stores (possibly through local slots/copies/non-negative
 * constant offsets), the tbuf header and dynamic allocation identity are
 * unobservable.  A fixed NYIR_ALLOCA therefore has the same observable
 * lifetime for the function and avoids allocator/oracle traffic in hot numeric
 * kernels.  We preserve the zero-filled allocation contract by clearing the
 * promoted payload at each original allocation site; this is important when
 * the allocation sat inside a loop and the same stack slot is reused.
 *
 * Calls/returns/PHIs and negative pointer offsets are deliberately rejected:
 * those cases need interprocedural escape/header proofs before stack promotion
 * is sound.  The narrow form already covers SHA message schedules and similar
 * scratch arrays while failing closed for general tbuf values.
 */
static bool nyr_tbuf_offset_guarded_before(const nyr_func_t *f, size_t at,
                                            int base, int offset) {
  if (!f || at > f->len || base < 0 || offset < 0)
    return false;
  for (size_t k = at; k > 0; --k) {
    const nyr_inst_t *in = &f->data[k - 1u];
    if (in->op == NYIR_BOUNDS_CHECK && in->a == base && in->b == offset)
      return true;
    if (in->op == NYIR_LABEL || in->op == NYIR_BR || in->op == NYIR_BR_IF ||
        in->op == NYIR_RET)
      break;
  }
  return false;
}

static bool nyr_tbuf_alloca_safe_uses(const nyr_func_t *f, size_t alloc_idx,
                                       int root, size_t bytes) {
  if (!f || root < 0 || root >= f->next_value)
    return false;
  size_t value_count = (size_t)f->next_value;
  size_t slot_count = nyr_local_slot_count(f);
  bool *derived = calloc(value_count ? value_count : 1u, sizeof(*derived));
  bool *slots = calloc(slot_count ? slot_count : 1u, sizeof(*slots));
  nyr_value_fact_t *facts =
      calloc(value_count ? value_count : 1u, sizeof(*facts));
  if (!derived || !slots || !facts) {
    free(derived);
    free(slots);
    free(facts);
    return false;
  }
  if (!nyr_analyze_values(f, facts, value_count, NULL, 0))
    memset(facts, 0, value_count * sizeof(*facts));
  derived[root] = true;

  bool changed = true;
  for (unsigned round = 0; changed && round < 32; ++round) {
    changed = false;
    for (size_t i = 0; i < f->len; ++i) {
      if (i == alloc_idx)
        continue;
      const nyr_inst_t *in = &f->data[i];
      if (in->op == NYIR_STORE_LOCAL && in->a >= 0 &&
          (size_t)in->a < value_count && derived[in->a] && in->imm >= 0 &&
          (size_t)in->imm < slot_count && !slots[in->imm]) {
        slots[in->imm] = true;
        changed = true;
      } else if (in->op == NYIR_LOAD_LOCAL && in->dst >= 0 &&
                 (size_t)in->dst < value_count && in->imm >= 0 &&
                 (size_t)in->imm < slot_count && slots[in->imm] &&
                 !derived[in->dst]) {
        derived[in->dst] = true;
        changed = true;
      } else if (in->op == NYIR_COPY && in->a >= 0 &&
                 (size_t)in->a < value_count && derived[in->a] &&
                 in->dst >= 0 && (size_t)in->dst < value_count &&
                 !derived[in->dst]) {
        derived[in->dst] = true;
        changed = true;
      } else if ((in->op == NYIR_ADD_I64 || in->op == NYIR_SUB_I64) &&
                 in->dst >= 0 && (size_t)in->dst < value_count && in->a >= 0 &&
                 (size_t)in->a < value_count && derived[in->a] &&
                 !derived[in->dst]) {
        int64_t off = 0;
        bool const_off =
            in->b >= 0 && nyr_tbuf_const_before(f, in->b, i, &off);
        bool safe_offset =
            const_off ? off >= 0
                      : nyr_tbuf_offset_guarded_before(f, i, in->a, in->b);
        if (!safe_offset && in->op == NYIR_ADD_I64 &&
            (in->flags & NYIR_INST_F_IN_BOUNDS_ADDR))
          safe_offset = true;
        if (!safe_offset && in->b >= 0 && bytes >= sizeof(int64_t)) {
          nyr_range_t r = {0};
          if (nir_value_range_at(f, facts, in->b, i, &r) && r.has_min &&
              r.has_max && r.min >= 0 &&
              (uint64_t)r.max <= (uint64_t)(bytes - sizeof(int64_t)))
            safe_offset = true;
        }
        if (safe_offset &&
            (in->op == NYIR_ADD_I64 || (const_off && off == 0))) {
          /*
           * ptr - positive would expose the tbuf header; never promote it.
           */
          derived[in->dst] = true;
          changed = true;
        }
      }
    }
  }

  bool safe = true;
  for (size_t i = 0; safe && i < f->len; ++i) {
    if (i == alloc_idx)
      continue;
    const nyr_inst_t *in = &f->data[i];

    /*
     * A pointer-carrying local may not be overwritten by an unrelated value.
     */
    if (in->op == NYIR_STORE_LOCAL && in->imm >= 0 &&
        (size_t)in->imm < slot_count && slots[in->imm]) {
      if (in->a < 0 || (size_t)in->a >= value_count || !derived[in->a]) {
        if (ny_trace_enabled("NY_TRACE_TBUF_ALLOCA"))
          fprintf(stderr,
                  "[tbuf-alloca] unsafe pc=%zu op=%s pointer-local overwrite\n",
                  i, nyr_op_name(in->op));
        safe = false;
        break;
      }
      continue;
    }
    if (in->op == NYIR_LOAD_LOCAL && in->imm >= 0 &&
        (size_t)in->imm < slot_count && slots[in->imm])
      continue;

    bool da = in->a >= 0 && (size_t)in->a < value_count && derived[in->a];
    bool db = in->b >= 0 && (size_t)in->b < value_count && derived[in->b];
    bool dc = in->c >= 0 && (size_t)in->c < value_count && derived[in->c];
    bool dd = in->d >= 0 && (size_t)in->d < value_count && derived[in->d];
    bool de = in->e >= 0 && (size_t)in->e < value_count && derived[in->e];
    bool df = in->f >= 0 && (size_t)in->f < value_count && derived[in->f];
    bool extra = false;
    for (size_t k = 0; k < in->extra_args_len; ++k)
      if (in->extra_args[k] >= 0 && (size_t)in->extra_args[k] < value_count &&
          derived[in->extra_args[k]]) {
        extra = true;
        break;
      }
    bool phi = false;
    for (size_t k = 0; k < in->phi_incoming_len; ++k)
      if (in->phi_incoming[k].value >= 0 &&
          (size_t)in->phi_incoming[k].value < value_count &&
          derived[in->phi_incoming[k].value]) {
        phi = true;
        break;
      }
    if (!(da || db || dc || dd || de || df || extra || phi))
      continue;

    switch (in->op) {
    case NYIR_COPY:
      safe = da && !db && !dc && !dd && !de && !df && !extra && !phi;
      break;
    case NYIR_ADD_I64:
    case NYIR_SUB_I64: {
      int64_t off = -1;
      bool const_off = in->b >= 0 && nyr_tbuf_const_before(f, in->b, i, &off);
      bool safe_offset =
          const_off ? off >= 0
                    : nyr_tbuf_offset_guarded_before(f, i, in->a, in->b);
      if (!safe_offset && in->op == NYIR_ADD_I64 &&
          (in->flags & NYIR_INST_F_IN_BOUNDS_ADDR))
        safe_offset = true;
      if (!safe_offset && in->b >= 0 && bytes >= sizeof(int64_t)) {
        nyr_range_t r = {0};
        if (nir_value_range_at(f, facts, in->b, i, &r) && r.has_min &&
            r.has_max && r.min >= 0 &&
            (uint64_t)r.max <= (uint64_t)(bytes - sizeof(int64_t)))
          safe_offset = true;
      }
      safe = da && !db && !dc && !dd && !de && !df && !extra && !phi &&
             safe_offset && (in->op == NYIR_ADD_I64 || (const_off && off == 0));
      break;
    }
    case NYIR_LOAD_I64:
      safe = da && !db && !dc && !dd && !de && !df && !extra && !phi;
      break;
    case NYIR_BOUNDS_CHECK:
      /*
       * The payload base is observational only; offset/length stay scalar.
       */
      safe = da && !db && !dc && !dd && !de && !df && !extra && !phi;
      break;
    case NYIR_STORE_I64:
      /*
       * a is address, c is value; never allow publishing the pointer.
       */
      safe = da && !db && !dc && !dd && !de && !df && !extra && !phi;
      break;
    case NYIR_STORE_LOCAL:
    case NYIR_LOAD_LOCAL:
      /*
       * handled by the local-slot checks above
       */
      break;
    case NYIR_CALL: {
      if (in->symbol) {
        const char *dot = strrchr(in->symbol, '.');
        const char *sym = dot ? dot + 1 : in->symbol;
        if (strcmp(sym, "sha256") == 0 || strcmp(sym, "sha1") == 0 ||
            strcmp(sym, "md5") == 0 || strcmp(sym, "hmac_sha256") == 0) {
          safe = !extra && !phi;
          break;
        }
      }
      safe = false;
      break;
    }
    default:
      safe = false;
      break;
    }
    if (!safe && ny_trace_enabled("NY_TRACE_TBUF_ALLOCA"))
      fprintf(stderr,
              "[tbuf-alloca] unsafe pc=%zu op=%s da=%d db=%d dc=%d dd=%d de=%d "
              "df=%d extra=%d phi=%d\n",
              i, nyr_op_name(in->op), da, db, dc, dd, de, df, extra, phi);
  }

  free(derived);
  free(slots);
  free(facts);
  return safe;
}

bool nyr_tbuf_promote_alloca(nyr_func_t *f) {
  if (!f || f->len == 0 || f->next_value < 0)
    return true;

  for (size_t i = 0; i < f->len; ++i) {
    nyr_inst_t *in = &f->data[i];
    if (in->op != NYIR_CALL || !in->symbol || in->dst < 0 ||
        strcmp(in->symbol, "rt_tbuf_new_typed_raw") != 0)
      continue;

    if (ny_trace_enabled("NY_TRACE_TBUF_ALLOCA"))
      fprintf(stderr, "[tbuf-alloca] candidate pc=%zu dst=v%d a=v%d b=v%d\n", i,
              in->dst, in->a, in->b);
    int64_t count = 0, elem_size = 0;
    if (in->a < 0 || in->b < 0 ||
        !nyr_tbuf_const_before(f, in->a, i, &count) ||
        !nyr_tbuf_const_before(f, in->b, i, &elem_size) || count <= 0 ||
        elem_size <= 0 || (uint64_t)count > SIZE_MAX / (uint64_t)elem_size) {
      if (ny_trace_enabled("NY_TRACE_TBUF_ALLOCA"))
        fprintf(stderr, "[tbuf-alloca] reject pc=%zu nonconst/invalid size\n",
                i);
      continue;
    }
    size_t bytes = (size_t)count * (size_t)elem_size;
    /*
     * Keep native frames bounded; larger workspaces stay in the arena.
     */
    if (bytes == 0 || bytes > 16u * 1024u || bytes > (size_t)INT64_MAX)
      continue;
    if (!nyr_tbuf_alloca_safe_uses(f, i, in->dst, bytes)) {
      if (ny_trace_enabled("NY_TRACE_TBUF_ALLOCA"))
        fprintf(stderr, "[tbuf-alloca] reject pc=%zu unsafe uses bytes=%zu\n",
                i, bytes);
      continue;
    }
    if (ny_trace_enabled("NY_TRACE_TBUF_ALLOCA"))
      fprintf(stderr, "[tbuf-alloca] promote pc=%zu bytes=%zu\n", i, bytes);
    if (f->next_value > INT_MAX - 2)
      return false;

    int root = in->dst;
    int zero_v = f->next_value++;
    int bytes_v = f->next_value++;
    nyr_debug_loc_t debug = in->debug;
    nyr_scalar_class_t scalar_class = in->scalar_class;
    uint8_t semantic_ownership = in->semantic_ownership;
    bool semantic_mutable = in->semantic_mutable;
    uint32_t alias_class = in->alias_class;

    /*
     * Preserve the original source/semantic location on the stack allocation.
     */
    nyr_inst_discard(in);
    f->data[i] = (nyr_inst_t){.op = NYIR_ALLOCA,
                               .dst = root,
                               .a = -1,
                               .b = -1,
                               .c = -1,
                               .d = -1,
                               .e = -1,
                               .f = -1,
                               .imm = (int64_t)bytes,
                               .scalar_class = scalar_class,
                               .semantic_ownership = semantic_ownership,
                               .semantic_mutable = semantic_mutable,
                               .alias_class = alias_class,
                               .debug = debug};
    f->data[i].effects = nyr_inst_effects(&f->data[i]);

    if (!nyr_ensure_insert(f, i + 1u, 3u))
      return false;
    f->data[i + 1u] = (nyr_inst_t){.op = NYIR_CONST_I64,
                                    .dst = zero_v,
                                    .a = -1,
                                    .b = -1,
                                    .c = -1,
                                    .d = -1,
                                    .e = -1,
                                    .f = -1,
                                    .imm = 0};
    f->data[i + 2u] = (nyr_inst_t){.op = NYIR_CONST_I64,
                                    .dst = bytes_v,
                                    .a = -1,
                                    .b = -1,
                                    .c = -1,
                                    .d = -1,
                                    .e = -1,
                                    .f = -1,
                                    .imm = (int64_t)bytes};
    f->data[i + 3u] = (nyr_inst_t){.op = NYIR_CALL,
                                    .dst = -1,
                                    .a = root,
                                    .b = zero_v,
                                    .c = bytes_v,
                                    .d = -1,
                                    .e = -1,
                                    .f = -1,
                                    .imm = 3,
                                    .flags = NYIR_INST_F_EXTERN,
                                    .symbol = "rt_memset"};
    f->has_symbol_calls = true;
    for (size_t k = i + 1u; k <= i + 3u; ++k)
      f->data[k].effects = nyr_inst_effects(&f->data[k]);
    i += 3u;
  }
  return true;
}

static int nyr_tbuf_private_store_value(const nyr_func_t *f,
                                         const nyr_cfg_t *cfg,
                                         size_t alloc_idx, int root,
                                         int64_t byte_offset, size_t load_idx) {
  if (!f || !cfg || byte_offset < 0 ||
      byte_offset > INT64_MAX - (int64_t)sizeof(int64_t) || load_idx >= f->len)
    return -1;
  size_t load_block = cfg->inst_block[load_idx];
  size_t store_idx = SIZE_MAX;
  int stored_value = -1;
  for (size_t i = alloc_idx + 1u; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->op != NYIR_STORE_I64 || in->a < 0 ||
        (size_t)in->a >= (size_t)f->next_value || in->c < 0 ||
        (size_t)in->c >= (size_t)f->next_value)
      continue;
    int64_t store_offset = 0;
    if (!nyr_tbuf_eval_const_offset(f, in->a, root, &store_offset, 0))
      continue;
    if (store_offset < 0 || store_offset > INT64_MAX - (int64_t)sizeof(int64_t))
      return -1;
    int64_t access_end = byte_offset + (int64_t)sizeof(int64_t);
    int64_t store_end = store_offset + (int64_t)sizeof(int64_t);
    if (store_end <= byte_offset || access_end <= store_offset)
      continue;
    if (store_offset != byte_offset || store_idx != SIZE_MAX)
      return -1;
    size_t store_block = cfg->inst_block[i];
    bool dominates = store_block == load_block
                         ? i < load_idx
                         : nyr_cfg_dominates(cfg, store_block, load_block);
    if (!dominates)
      return -1;
    store_idx = i;
    stored_value = in->c;
  }
  return stored_value;
}

/*
 * Remove a private dynamic list object when optimization has already proven
 * that every use is only address formation followed by stores.  This is the
 * named-temporary form of escape scalar replacement: the value loaded from
 * `p[0]` may already have been forwarded, leaving only the allocation and its
 * dead initialization stores behind.
 */
bool nyr_tbuf_private_object(nyr_func_t *f) {
  if (!f || f->len == 0 || f->next_value <= 0)
    return true;
  nyr_use_def_t uses = {0};
  if (!nyr_build_use_def(f, &uses))
    return false;
  size_t value_count = (size_t)f->next_value;
  for (size_t alloc_idx = 0; alloc_idx < f->len; ++alloc_idx) {
    nyr_inst_t *alloc = &f->data[alloc_idx];
    if (alloc->op != NYIR_CALL || !alloc->symbol ||
        strcmp(alloc->symbol, "rt_tbuf_new_raw") != 0 || alloc->dst < 0 ||
        (size_t)alloc->dst >= value_count)
      continue;
    int64_t elem_size = 0;
    if (alloc->b < 0 ||
        !nyr_tbuf_const_before(f, alloc->b, alloc_idx, &elem_size) ||
        elem_size < (int64_t)sizeof(int64_t))
      continue;
    bool *derived = calloc(value_count, sizeof(*derived));
    bool *seen = calloc(f->len, sizeof(*seen));
    int *queue = malloc(value_count * sizeof(*queue));
    /*
     * Stage forwarding rewrites until the complete use graph is proven safe.
     * A rejected candidate must leave the input IR byte-for-byte unchanged.
     */
    int *rewrite_to = malloc(f->len * sizeof(*rewrite_to));
    if (!derived || !seen || !queue || !rewrite_to) {
      free(derived);
      free(seen);
      free(queue);
      free(rewrite_to);
      nyr_use_def_free(&uses);
      return false;
    }
    nyr_cfg_t cfg = {0};
    if (!nyr_cfg_build(f, &cfg)) {
      free(derived);
      free(seen);
      free(queue);
      free(rewrite_to);
      nyr_use_def_free(&uses);
      return false;
    }
    for (size_t i = 0; i < f->len; ++i)
      rewrite_to[i] = -1;
    size_t queue_len = 0, queue_pos = 0;
    bool has_store = false;
    derived[alloc->dst] = true;
    queue[queue_len++] = alloc->dst;
    bool safe = true;
    const char *reject_reason = NULL;
    size_t reject_use = 0;
    while (safe && queue_pos < queue_len) {
      int value = queue[queue_pos++];
      size_t begin = uses.offsets[value];
      size_t end = uses.offsets[value + 1];
      for (size_t ui = begin; ui < end && safe; ++ui) {
        size_t user_idx = uses.users[ui];
        if (user_idx == alloc_idx || user_idx >= f->len)
          continue;
        nyr_inst_t *in = &f->data[user_idx];
        bool a = in->a == value;
        bool b = in->b == value;
        bool c = in->c == value;
        if (in->op == NYIR_COPY && a && !b && !c && in->dst >= 0 &&
            (size_t)in->dst < value_count) {
          if (!derived[in->dst]) {
            derived[in->dst] = true;
            queue[queue_len++] = in->dst;
          }
        } else if ((in->op == NYIR_ADD_I64 || in->op == NYIR_SUB_I64 ||
                    in->op == NYIR_MUL_I64 || in->op == NYIR_SHL_I64) &&
                   (a != b) && (a || b) && in->dst >= 0 &&
                   (size_t)in->dst < value_count) {
          int other = a ? in->b : in->a;
          int64_t constant = 0;
          if (other < 0 ||
              !nyr_tbuf_const_before(f, other, user_idx, &constant)) {
            safe = false;
            reject_reason = "derived address has non-constant offset";
            reject_use = user_idx;
            break;
          }
          if (in->op == NYIR_SHL_I64 && (constant < 0 || constant >= 63)) {
            safe = false;
            reject_reason = "derived address shift is outside 0..62";
            reject_use = user_idx;
            break;
          }
          /*
           * A length query is rewritten below to the allocation's count.
           * Following count * element_size lets us recognize the byte
           * address generated for `p[index]` without treating arbitrary
           * pointer arithmetic as a private-buffer use.
           */
          if (!derived[in->dst]) {
            derived[in->dst] = true;
            queue[queue_len++] = in->dst;
          }
        } else if (in->op == NYIR_STORE_I64 && a && in->c >= 0 &&
                   !derived[in->c]) {
          seen[user_idx] = true;
          has_store = true;
        } else if (in->op == NYIR_CALL && a && !c && in->symbol &&
                   strcmp(in->symbol, "rt_tbuf_index_read_raw") == 0) {
          /*
           * A private list read is just a load from the element address when
           * exactly one matching store dominates it on every control-flow
           * path.  Forward that value so the allocation and runtime helpers
           * can be removed by scalar replacement.
           */
          int64_t index = 0, read_elem_size = 0, byte_offset = 0;
          if (in->b < 0 ||
              !nyr_tbuf_const_before(f, in->b, user_idx, &index) ||
              index < 0 || alloc->b < 0 ||
              !nyr_tbuf_const_before(f, alloc->b, alloc_idx,
                                      &read_elem_size) ||
              read_elem_size != (int64_t)sizeof(int64_t) ||
              index > INT64_MAX / read_elem_size) {
            safe = false;
            reject_reason =
                "private index read is not a known nonnegative index";
            reject_use = user_idx;
          } else {
            byte_offset = index * read_elem_size;
            int stored_val = nyr_tbuf_private_store_value(
                f, &cfg, alloc_idx, alloc->dst, byte_offset, user_idx);
            if (stored_val >= 0 && in->dst >= 0)
              rewrite_to[user_idx] = stored_val;
            else
              safe = false,
              reject_reason = "private index read has no matching prior store",
              reject_use = user_idx;
          }
        } else if (in->op == NYIR_CALL && a && !b && !c && in->symbol &&
                   strcmp(in->symbol, "rt_tbuf_len_raw") == 0) {
          int64_t off = 0;
          /*
           * A private tbuf whose only operations are address formation,
           * payload stores/loads and length reads cannot have its count
           * changed.  Replace an exact-base length query with the allocator's
           * count SSA value.  Do not mark the rewritten COPY as dead: it
           * defines the original call result just like forwarded loads below.
           */
          if (nyr_tbuf_eval_const_offset(f, in->a, alloc->dst, &off, 0) &&
              off == 0 && alloc->a >= 0) {
            rewrite_to[user_idx] = alloc->a;
            if (in->dst >= 0 && (size_t)in->dst < value_count &&
                !derived[in->dst]) {
              derived[in->dst] = true;
              queue[queue_len++] = in->dst;
            }
            seen[user_idx] = true;
          } else {
            safe = false;
            reject_reason = "length query is not at the exact allocation base";
            reject_use = user_idx;
          }
        } else if (in->op == NYIR_LOAD_I64 && a) {
          int64_t byte_offset = 0;
          int stored_val =
              nyr_tbuf_eval_const_offset(f, in->a, alloc->dst, &byte_offset, 0)
                  ? nyr_tbuf_private_store_value(
                        f, &cfg, alloc_idx, alloc->dst, byte_offset, user_idx)
                  : -1;
          if (stored_val >= 0) {
            /*
             * Keep the forwarded COPY: it defines the original load result.
             * Only the dead object/address/store scaffolding is discarded.
             */
            rewrite_to[user_idx] = stored_val;
          } else {
            safe = false;
            reject_reason = "private load has no matching prior store";
            reject_use = user_idx;
          }
        } else if (in->op == NYIR_STORE_LOCAL && a && in->imm >= 0) {
          seen[user_idx] = true;
          int64_t slot = in->imm;
          for (size_t k = 0; k < f->len; ++k) {
            if (f->data[k].op == NYIR_LOAD_LOCAL && f->data[k].imm == slot &&
                f->data[k].dst >= 0 && (size_t)f->data[k].dst < value_count) {
              seen[k] = true;
              if (!derived[f->data[k].dst]) {
                derived[f->data[k].dst] = true;
                queue[queue_len++] = f->data[k].dst;
              }
            }
          }
        } else if (in->op == NYIR_BOUNDS_CHECK && (a || b || c)) {
          int64_t count = 0, bounds_elem_size = 0, byte_offset = 0,
                  base_off = 0;
          int64_t check_limit = 0;
          int64_t access_width = 0;
          uint64_t capacity = 0;
          bool bounds_proven =
              alloc->a >= 0 && alloc->b >= 0 && in->b >= 0 &&
              nyr_tbuf_const_before(f, alloc->a, alloc_idx, &count) &&
              nyr_tbuf_const_before(f, alloc->b, alloc_idx,
                                     &bounds_elem_size) &&
              nyr_tbuf_const_before(f, in->b, user_idx, &byte_offset) &&
              nyr_tbuf_eval_const_offset(f, in->a, alloc->dst, &base_off, 0) &&
              (in->c < 0 ? (check_limit = in->imm, true)
                         : nyr_tbuf_eval_private_length(
                               f, in->c, alloc, alloc->dst, &check_limit, 0)) &&
              count >= 0 && bounds_elem_size > 0 && byte_offset >= 0 &&
              base_off >= 0 && check_limit >= 0 &&
              (uint64_t)count <= UINT64_MAX / (uint64_t)bounds_elem_size;
          /*
           * BOUNDS_CHECK's imm/c operands are the logical byte length, not
           * the memory operation's width.  This scalar-replacement path only
           * forwards NYIR_LOAD_I64/STORE_I64 slots, so prove the full i64
           * access against the allocation instead of treating the length as
           * a second access width.
           */
          access_width = (int64_t)sizeof(int64_t);
          bounds_proven = bounds_proven && access_width > 0;
          if (bounds_proven) {
            capacity = (uint64_t)count * (uint64_t)bounds_elem_size;
            uint64_t effective = 0;
            bounds_proven =
                (uint64_t)base_off <= UINT64_MAX - (uint64_t)byte_offset;
            if (bounds_proven)
              effective = (uint64_t)base_off + (uint64_t)byte_offset;
            bounds_proven =
                bounds_proven && (uint64_t)check_limit <= capacity &&
                effective <= (uint64_t)check_limit &&
                (uint64_t)access_width <= (uint64_t)check_limit - effective;
          }
          if (bounds_proven)
            seen[user_idx] = true;
          else {
            if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
              ny_dbg_heuristic_event(
                  "SROA_BOUNDS",
                  "reject alloc=v%d check=i%zu value=v%d "
                  "operands={base:v%d,offset:v%d,length:v%d} width=%lld "
                  "count=%lld elem=%lld offset=%lld base=%lld limit=%lld "
                  "proof=%d",
                  alloc->dst, user_idx, value, in->a, in->b, in->c,
                  (long long)access_width, (long long)count,
                  (long long)bounds_elem_size, (long long)byte_offset,
                  (long long)base_off, (long long)check_limit,
                  (int)bounds_proven);
            safe = false;
          }
        } else {
          safe = false;
          reject_reason = "allocation-derived value reaches an unsupported use";
          reject_use = user_idx;
        }
        if (safe && in->dst >= 0 && (size_t)in->dst < value_count &&
            derived[in->dst] &&
            (in->op == NYIR_COPY || in->op == NYIR_ADD_I64 ||
             in->op == NYIR_SUB_I64))
          seen[user_idx] = true;
      }
    }
    if (safe && has_store) {
      for (size_t i = 0; i < f->len; ++i)
        if (rewrite_to[i] >= 0)
          nir_make_copy(&f->data[i], rewrite_to[i]);
      for (size_t i = 0; i < f->len; ++i)
        if (i == alloc_idx || seen[i] ||
            (f->data[i].dst >= 0 && (size_t)f->data[i].dst < value_count &&
             derived[f->data[i].dst]))
          nyr_inst_discard(&f->data[i]);
    }
    free(derived);
    free(seen);
    free(queue);
    free(rewrite_to);
    nyr_cfg_free(&cfg);
    if (!safe || !has_store) {
      if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
        ny_dbg_heuristic_event(
            "SROA",
            "tbuf-private reject alloc=i%zu v%d reason=%s use=i%zu op=%s",
            alloc_idx, alloc->dst,
            reject_reason ? reject_reason : "no payload store", reject_use,
            reject_use < f->len ? nyr_op_name(f->data[reject_use].op)
                                : "<none>");
      continue;
    }
  }
  nyr_use_def_free(&uses);
  return true;
}

/*
 * Apply NyP1 profile: boost inline candidate size when edge density is high.
 */
static uint64_t ny_profile_edges;
static uint64_t ny_profile_steps;

void ny_native_profile_set_runtime(uint64_t edges, uint64_t steps) {
  ny_profile_edges = edges;
  ny_profile_steps = steps;
}

bool ny_native_profile_should_inline(size_t callee_insts) {
  /*
   * Hot profiles allow larger callees (up to 32 insts). NyP-style density.
   */
  size_t lim = 12;
  if (ny_profile_steps > 10000 && ny_profile_edges > 100)
    lim = 32;
  else if (ny_profile_steps > 1000)
    lim = 20;
  return callee_insts > 0 && callee_insts <= lim;
}

/*
 * Dense NyP-style ICP: count monomorphic direct CALL targets in the function.
 * When a single symbol dominates (>= 2 sites, or unique among multi-call),
 * boost profile so inline_small admits that callee's body.
 */
bool nyr_icp_profile(nyr_func_t *f) {
  if (!f || f->len == 0)
    return true;
  /*
   * Collect up to 16 distinct direct call symbols and counts.
   */
  const char *syms[16];
  unsigned counts[16];
  size_t nsym = 0;
  size_t total_calls = 0;
  for (size_t i = 0; i < f->len; ++i) {
    if (f->data[i].op != NYIR_CALL || !f->data[i].symbol)
      continue;
    total_calls++;
    const char *s = f->data[i].symbol;
    size_t j;
    for (j = 0; j < nsym; ++j)
      if (strcmp(syms[j], s) == 0) {
        counts[j]++;
        break;
      }
    if (j == nsym && nsym < 16) {
      syms[nsym] = s;
      counts[nsym] = 1;
      nsym++;
    }
  }
  if (total_calls == 0)
    return true;
  /*
   * Monomorphic: one target, or one target has >= 2/3 of direct calls.
   */
  unsigned best = 0;
  for (size_t j = 0; j < nsym; ++j)
    if (counts[j] > best)
      best = counts[j];
  bool mono = (nsym == 1 && total_calls >= 1) ||
              (best * 3 >= (unsigned)total_calls * 2 && best >= 2);
  if (mono) {
    uint64_t steps = ny_profile_steps < 2000 ? 2000 : ny_profile_steps;
    uint64_t edges =
        ny_profile_edges < 200 ? 200 : ny_profile_edges + best * 10;
    ny_native_profile_set_runtime(edges, steps);
  } else if (ny_profile_steps >= 100 && ny_profile_edges < 200) {
    ny_native_profile_set_runtime(ny_profile_edges + 100, ny_profile_steps);
  }
  return true;
}

uint64_t ny_native_profile_edges(void) { return ny_profile_edges; }
uint64_t ny_native_profile_steps(void) { return ny_profile_steps; }

/*
 * Aggregate/field SROA: delegates to the unified multi-tier SROA engine.
 */
bool nyr_aggregate_sroa(nyr_func_t *f) { return nyr_sroa_unified(f); }

/*
 * Dead store after store to same non-escaped local with no intervening load.
 */
bool nyr_store_sink(nyr_func_t *f) { return nyr_sroa_unified(f); }

/*
 * Local points-to SROA: delegates to the unified multi-tier SROA engine.
 */
bool nyr_points_to_sroa(nyr_func_t *f) { return nyr_sroa_unified(f); }

static void nyr_polyhedral_descending_ranges(nyr_func_t *f) {
  if (!f || f->len < 8)
    return;
  for (size_t cmp_i = 0; cmp_i < f->len; ++cmp_i) {
    nyr_inst_t *cmp = &f->data[cmp_i];
    if (cmp->op != NYIR_CMP_I64 || cmp->cmp != NYIR_CMP_GT || cmp->a < 0 ||
        cmp->b < 0)
      continue;
    int iv = cmp->a;
    int64_t limit = 0;
    bool limit_known = false;
    for (size_t j = 0; j < f->len; ++j) {
      if (f->data[j].dst == cmp->b && f->data[j].op == NYIR_CONST_I64) {
        limit = f->data[j].imm;
        limit_known = true;
        break;
      }
    }
    if (!limit_known)
      continue;

    int back_value = -1;
    int64_t entry = 0;
    bool entry_known = false;
    for (size_t j = 0; j < cmp_i; ++j) {
      nyr_inst_t *phi = &f->data[j];
      if (phi->op != NYIR_PHI || phi->dst != iv || phi->phi_incoming_len != 2 ||
          !phi->phi_incoming)
        continue;
      for (size_t k = 0; k < phi->phi_incoming_len; ++k) {
        int v = phi->phi_incoming[k].value;
        bool is_const = false;
        int64_t c = 0;
        for (size_t m = 0; m < f->len; ++m) {
          if (f->data[m].dst != v)
            continue;
          if (f->data[m].op == NYIR_CONST_I64) {
            is_const = true;
            c = f->data[m].imm;
          }
          break;
        }
        if (is_const) {
          entry = c;
          entry_known = true;
        } else if (back_value < 0) {
          back_value = v;
        }
      }
      break;
    }
    if (!entry_known || back_value < 0 || entry <= limit)
      continue;

    int64_t step = 0;
    bool step_known = false;
    for (size_t j = 0; j < f->len; ++j) {
      const nyr_inst_t *upd = &f->data[j];
      if (upd->dst != back_value)
        continue;
      int step_v = -1;
      if (upd->op == NYIR_SUB_I64 && upd->a == iv)
        step_v = upd->b;
      else if (upd->op == NYIR_ADD_I64) {
        if (upd->a == iv)
          step_v = upd->b;
        else if (upd->b == iv)
          step_v = upd->a;
      }
      if (step_v < 0)
        break;
      for (size_t m = 0; m < f->len; ++m) {
        if (f->data[m].dst != step_v || f->data[m].op != NYIR_CONST_I64)
          continue;
        int64_t c = f->data[m].imm;
        if (upd->op == NYIR_SUB_I64 && c > 0) {
          step = c;
          step_known = true;
        } else if (upd->op == NYIR_ADD_I64 && c < 0 && c != INT64_MIN) {
          step = -c;
          step_known = true;
        }
        break;
      }
      break;
    }
    if (!step_known || step <= 0)
      continue;

    __int128 span = (__int128)entry - (__int128)limit;
    __int128 rounds = (span + step - 1) / step;
    __int128 fail = (__int128)entry - rounds * step;
    if (fail < INT64_MIN || fail > INT64_MAX)
      continue;
    int64_t iv_min = (int64_t)fail;
    for (size_t j = 0; j < f->len; ++j) {
      if (f->data[j].dst != iv && f->data[j].dst != back_value)
        continue;
      if (f->data[j].op != NYIR_PHI && f->data[j].op != NYIR_COPY &&
          f->data[j].op != NYIR_SUB_I64 && f->data[j].op != NYIR_ADD_I64)
        continue;
      f->data[j].range.has_min = true;
      f->data[j].range.has_max = true;
      f->data[j].range.min = iv_min;
      f->data[j].range.max = entry;
    }
    cmp->range =
        (nyr_range_t){.has_min = true, .has_max = true, .min = 0, .max = 1};
  }
}

/*
 * Polyhedral-style nest analysis: counted outer/inner trip, IV ranges, and
 * dependence-free tiling hint (when both trips constant and body is pure
 * affine add/mul on IVs — mark outer IV range so unroll can fire).
 */
bool nyr_polyhedral_nest(nyr_func_t *f) {
  nyr_polyhedral_descending_ranges(f);
  if (!f || f->len < 12)
    return true;
  for (size_t i = 0; i < f->len; ++i) {
    if (f->data[i].op != NYIR_CMP_I64 || f->data[i].cmp != NYIR_CMP_LT)
      continue;
    int iv = f->data[i].a;
    int lim = f->data[i].b;
    if (iv < 0 || lim < 0)
      continue;
    int64_t trip = -1;
    for (size_t j = 0; j < i; ++j)
      if (f->data[j].op == NYIR_CONST_I64 && f->data[j].dst == lim) {
        trip = f->data[j].imm;
        break;
      }
    if (trip < 1 || trip > 1000000)
      continue;
    f->data[i].range.has_min = true;
    f->data[i].range.has_max = true;
    f->data[i].range.min = 0;
    f->data[i].range.max = 1;
    /*
     * Tag the IV defs only for a canonical +1-step induction with a proven
     * non-negative entry value.  The header phi legitimately reaches `trip`
     * on the exit-check iteration (`i == lim` fails `i < lim`), so the sound
     * max is `trip`, never `trip - 1`: a too-low max lets peephole's range
     * fold prove the exit test `i >= lim` always false and delete the loop
     * guard (infinite loop / skipped loop after inlining clones the ranges).
     * Tag the IV defs only for a canonical +1-step induction with a proven
     * non-negative constant start.  The header phi legitimately reaches
     * `trip` on the exit-check iteration (`i == lim` fails `i < lim`), so the
     * sound max is `trip`, never `trip - 1`: a too-low max lets peephole's
     * range fold prove the exit test `i >= lim` always false and delete the
     * loop guard (infinite loop / skipped loop once inlining clones the
     * ranges into the caller).
     *
     * The step add lives in the loop body, AFTER the header's CMP in linear
     * IR order, so the whole function is scanned for the phi's backedge
     * operand instead of only scanning before the CMP.
     */
    int64_t entry_imm = 0;
    bool entry_known = false;
    int64_t step_imm = 0;
    bool positive_step = false;
    int back_op = -1;
    /*
     * Modern NYIR PHIs carry predecessor/value pairs exclusively in
     * phi_incoming; a/b are normalized to -1.  Reading a/b here made IV range
     * discovery silently fail for every SSA-built loop.
     */
    for (size_t j = 0; j < i; ++j) {
      nyr_inst_t *phi = &f->data[j];
      if (phi->dst != iv || phi->op != NYIR_PHI || phi->phi_incoming_len != 2 ||
          !phi->phi_incoming)
        continue;
      for (size_t k = 0; k < phi->phi_incoming_len; ++k) {
        int op = phi->phi_incoming[k].value;
        if (op < 0 || op >= f->next_value)
          continue;
        bool is_const = false;
        int64_t cval = 0;
        for (size_t m = 0; m < f->len; ++m) {
          if (f->data[m].dst != op)
            continue;
          if (f->data[m].op == NYIR_CONST_I64) {
            is_const = true;
            cval = f->data[m].imm;
          }
          break;
        }
        if (is_const && cval >= 0) {
          entry_imm = cval;
          entry_known = true;
        } else if (!is_const && back_op < 0) {
          back_op = op;
        }
      }
      if (entry_known && back_op >= 0) {
        for (size_t m = 0; m < f->len && !positive_step; ++m) {
          const nyr_inst_t *st = &f->data[m];
          if (st->dst != back_op || st->op != NYIR_ADD_I64)
            continue;
          int c_op = st->a == iv ? st->b : (st->b == iv ? st->a : -1);
          if (c_op < 0)
            continue;
          for (size_t n = 0; n < f->len; ++n) {
            if (f->data[n].op != NYIR_CONST_I64 || f->data[n].dst != c_op ||
                f->data[n].imm <= 0)
              continue;
            step_imm = f->data[n].imm;
            positive_step = true;
            break;
          }
        }
      }
      break;
    }
    /*
     * For iv = entry + k*step with iv < limit, the header PHI can reach one
     * failing value at/above the limit.  Compute that exact aligned value
     * rather than assuming unit stride, and attach the same range to the
     * backedge update so derived affine expressions inherit it.
     */
    if (entry_known && positive_step) {
      int64_t iv_max = entry_imm;
      if (entry_imm < trip) {
        uint64_t span = (uint64_t)trip - (uint64_t)entry_imm;
        uint64_t step = (uint64_t)step_imm;
        uint64_t rounds = span / step + (span % step != 0);
        if (rounds <= (uint64_t)INT64_MAX / step) {
          uint64_t advance = rounds * step;
          if (advance <= (uint64_t)INT64_MAX - (uint64_t)entry_imm)
            iv_max = entry_imm + (int64_t)advance;
          else
            positive_step = false;
        } else {
          positive_step = false;
        }
      }
      if (positive_step) {
        for (size_t j = 0; j < f->len; ++j) {
          if (f->data[j].dst != iv && f->data[j].dst != back_op)
            continue;
          if (f->data[j].op == NYIR_COPY || f->data[j].op == NYIR_ADD_I64 ||
              f->data[j].op == NYIR_PHI) {
            f->data[j].range.has_min = true;
            f->data[j].range.has_max = true;
            f->data[j].range.min = entry_imm;
            f->data[j].range.max = iv_max;
          }
        }
      }
    }
    int64_t trip2 = -1;
    for (size_t k = i + 1; k < f->len && k < i + 128; ++k) {
      if (f->data[k].op != NYIR_CMP_I64 || f->data[k].cmp != NYIR_CMP_LT)
        continue;
      int lim2 = f->data[k].b;
      if (lim2 < 0)
        continue;
      for (size_t j = 0; j < k; ++j)
        if (f->data[j].op == NYIR_CONST_I64 && f->data[j].dst == lim2) {
          trip2 = f->data[j].imm;
          break;
        }
      if (trip2 >= 1 && trip2 <= 10000) {
        f->data[k].range.has_min = true;
        f->data[k].range.has_max = true;
        f->data[k].range.min = 0;
        f->data[k].range.max = 1;
        break;
      }
    }
    /*
     * Tiling / unroll hint: small constant trip or nest product ≤ 64 →
     * tag lim const with range so consumers see exact trip; boost profile
     * when product is tile-friendly (power-of-two-ish small).
     */
    if (trip >= 2 && trip <= 8) {
      for (size_t j = 0; j < i; ++j)
        if (f->data[j].op == NYIR_CONST_I64 && f->data[j].dst == lim) {
          f->data[j].range.has_min = true;
          f->data[j].range.has_max = true;
          f->data[j].range.min = trip;
          f->data[j].range.max = trip;
        }
    }
    if (trip2 >= 2 && trip >= 2 && trip * trip2 <= 64) {
      if (ny_profile_steps < 3000)
        ny_native_profile_set_runtime(ny_profile_edges + 20, 3000);
    }
  }
  return true;
}

/*
 * @kernel autotune: CALL targets matching *kernel* → larger inline budget,
 * profile boost proportional to arg-count (proxy for work item), and
 * polyhedral-friendly step count so nest ranges fire before inline.
 */
bool nyr_kernel_hint(nyr_func_t *f) {
  if (!f)
    return true;
  size_t kernel_calls = 0;
  size_t kernel_args = 0;
  for (size_t i = 0; i < f->len; ++i) {
    nyr_inst_t *in = &f->data[i];
    if (in->op != NYIR_CALL || !in->symbol)
      continue;
    if (strstr(in->symbol, "kernel") || strstr(in->symbol, "Kernel") ||
        strstr(in->symbol, "KERNEL")) {
      kernel_calls++;
      int args[16];
      int argc = 0;
      if (nyr_call_args(in, f->next_value, args, 16, &argc, NULL, 0))
        kernel_args += (size_t)(argc > 0 ? argc : 1);
      else
        kernel_args += 1;
    }
  }
  if (kernel_calls == 0)
    return true;
  /*
   * Target pick: more args / more sites → hotter kernel → denser budget.
   */
  uint64_t steps = 5000 + kernel_calls * 500 + kernel_args * 100;
  if (steps > 20000)
    steps = 20000;
  uint64_t edges = ny_profile_edges + 50 + kernel_calls * 25;
  if (ny_profile_steps < steps)
    ny_native_profile_set_runtime(edges, steps);
  return true;
}

/*
 * Profile-heat block layout.
 *
 * When a native profile is loaded, this pass physically reorders basic blocks
 * in the linear instruction stream so that hot blocks and hot edges sit
 * earlier and adjacent, improving I-cache locality and branch-predictor
 * fall-through.
 *
 * Correctness invariants, all preserved:
 *   - Every block body is moved verbatim (leading label, body, one terminator),
 *     so all branch targets and PHI predecessor labels keep their identity.
 *   - The new block order is validated as a topological order over the
 *     cross-block SSA def-use relation, so no definition is ever placed after
 *     a use (dominance preserved).  If the greedy hot order violates this, the
 *     pass keeps the original layout.
 *   - The transform commits only after `nyr_ir_verify` accepts the result;
 *     otherwise the function is left byte-for-byte unchanged.
 *   - Without a loaded profile this pass is a strict no-op.
 */
static uint64_t nyr_layout_block_hot(const nyr_cfg_t *cfg, size_t block) {
  uint64_t hot = 0;
  for (size_t i = cfg->block_start[block]; i < cfg->block_end[block]; ++i)
    hot += ny_native_profile_block_hot(i);
  return hot;
}

static bool nyr_layout_blocks_are_ordered(size_t a, size_t b,
                                           const size_t *order,
                                           size_t block_count) {
  for (size_t i = 0; i < block_count; ++i)
    if (order[i] == a)
      return true;
    else if (order[i] == b)
      return false;
  return false;
}

/*
 * The fallthrough successor of a terminating BR_IF: the CFG successor whose
 * label does not match the branch target.  Returns SIZE_MAX when the layout
 * has no distinct fallthrough edge.
 */
static size_t nyr_layout_fallthrough_succ(const nyr_func_t *f,
                                           const nyr_cfg_t *cfg, size_t block,
                                           const nyr_inst_t *term) {
  for (size_t e = cfg->succ_offsets[block]; e < cfg->succ_offsets[block + 1];
       ++e) {
    size_t s = cfg->succ_blocks[e];
    bool is_target = false;
    for (size_t j = cfg->block_start[s]; j < cfg->block_end[s]; ++j) {
      if (f->data[j].op == NYIR_LABEL && f->data[j].imm == term->imm) {
        is_target = true;
        break;
      }
    }
    if (!is_target)
      return s;
  }
  return SIZE_MAX;
}

/*
 * The canonical lowering of a source conditional is a BR_IF followed by an
 * unlabeled BR for its false edge.  CFG construction quite correctly makes
 * that BR its own fallthrough block, but block layout cannot branch to an
 * unlabeled block after reordering.  Its terminator is the actual false-arm
 * label, so use that label when materialising the now-non-fallthrough edge.
 */
static int64_t nyr_layout_block_label(const nyr_func_t *f,
                                       const nyr_cfg_t *cfg, size_t block) {
  if (!f || !cfg || block >= cfg->block_count)
    return -1;
  for (size_t i = cfg->block_start[block]; i < cfg->block_end[block]; ++i)
    if (f->data[i].op == NYIR_LABEL)
      return f->data[i].imm;
  size_t end = cfg->block_end[block];
  while (end > cfg->block_start[block] && f->data[end - 1].op == NYIR_NOP)
    --end;
  if (end > cfg->block_start[block] && f->data[end - 1].op == NYIR_BR)
    return f->data[end - 1].imm;
  return -1;
}

bool nyr_block_layout(nyr_func_t *f) {
  if (!f || f->len == 0 || ny_native_profile_steps() == 0)
    return true;

  nyr_cfg_t cfg = {0};
  if (!nyr_cfg_build(f, &cfg)) {
    nyr_cfg_free(&cfg);
    return false;
  }

  const size_t blocks = cfg.block_count;
  if (blocks < 3) {
    nyr_cfg_free(&cfg);
    return true;
  }

  for (size_t b = 0; b < blocks; ++b) {
    size_t end = cfg.block_end[b];
    if (end == cfg.block_start[b])
      continue;
    nyr_op_t last_op = f->data[end - 1].op;
    /*
     * Conditional branches are permitted: the rebuild stage below inserts an
     * explicit `br` for the fallthrough successor whenever the reordering
     * moves it off the linear path, preserving semantics.
     */
    if (last_op != NYIR_BR && last_op != NYIR_RET && last_op != NYIR_BR_IF) {
      nyr_cfg_free(&cfg);
      return true;
    }
  }

  /*
   * Build a per-block heat and a greedy hot-path order: repeatedly step from
   * the current block to its hottest unvisited successor; when no successor is
   * unvisited, jump to the hottest unvisited block.  Block 0 always opens the
   * layout (the entry block need not carry a label and must stay first).
   */
  uint64_t *heat = ny_calloc_array(blocks, sizeof(*heat));
  bool *done = ny_calloc_array(blocks, sizeof(*done));
  size_t *order = ny_malloc_array(blocks, sizeof(*order));
  if (!heat || !done || !order) {
    free(heat);
    free(done);
    free(order);
    nyr_cfg_free(&cfg);
    return false;
  }
  bool has_block_heat = false;
  for (size_t b = 0; b < blocks; ++b) {
    heat[b] = nyr_layout_block_hot(&cfg, b);
    has_block_heat |= heat[b] != 0;
  }
  /*
   * Synthetic ICP/kernel hints can make the global profile step estimate
   * nonzero, but they do not provide per-block execution counts.  In that
   * case every block ties at zero and the greedy walk still changes layout
   * based on incidental CFG successor order.  Keep the original layout until
   * this function has real heat data; profile absence must not trigger a
   * semantics-sensitive reorder.
   */
  if (!has_block_heat) {
    free(heat);
    free(done);
    free(order);
    nyr_cfg_free(&cfg);
    return true;
  }

  size_t placed = 0;
  order[placed++] = 0;
  done[0] = true;
  size_t cur = 0;
  while (placed < blocks) {
    size_t next = SIZE_MAX;
    uint64_t best = 0;
    for (size_t e = cfg.succ_offsets[cur]; e < cfg.succ_offsets[cur + 1]; ++e) {
      size_t succ = cfg.succ_blocks[e];
      if (done[succ] || cfg.backedge_edges[e])
        continue;
      if (heat[succ] > best || (heat[succ] == best && next == SIZE_MAX)) {
        best = heat[succ];
        next = succ;
      }
    }
    if (next == SIZE_MAX) {
      best = 0;
      for (size_t b = 0; b < blocks; ++b) {
        if (done[b])
          continue;
        if (heat[b] > best || (heat[b] == best && next == SIZE_MAX)) {
          best = heat[b];
          next = b;
        }
      }
    }
    if (next == SIZE_MAX) {
      /*
       * All remaining blocks unreachable from the walk; append in order.
       */
      for (size_t b = 0; b < blocks; ++b)
        if (!done[b])
          order[placed++] = b;
      break;
    }
    order[placed++] = next;
    done[next] = true;
    cur = next;
  }

  /*
   * Validate the order is a topological sort of cross-block def-use edges.
   */
  bool topo_ok = true;
  int *block_of_value = NULL;
  {
    int *block_of_def = ny_calloc_array(
        (size_t)(f->next_value > 0 ? f->next_value : 1), sizeof(*block_of_def));
    if (!block_of_def) {
      free(heat);
      free(done);
      free(order);
      nyr_cfg_free(&cfg);
      return false;
    }
    for (size_t i = 0; i < f->len; ++i) {
      const nyr_inst_t *in = &f->data[i];
      if (in->dst >= 0 && in->dst < f->next_value)
        block_of_def[in->dst] = (int)cfg.inst_block[i];
    }
    block_of_value = block_of_def;
  }
  for (size_t i = 0; i < f->len && topo_ok; ++i) {
    const nyr_inst_t *in = &f->data[i];
    size_t use_block = cfg.inst_block[i];
    int defs[8];
    size_t defc = 0;
    if (in->op == NYIR_PHI && in->phi_incoming) {
      for (size_t k = 0; k < in->phi_incoming_len; ++k)
        if (in->phi_incoming[k].value >= 0 && defc < 8)
          defs[defc++] = in->phi_incoming[k].value;
    } else {
      const int vals[6] = {in->a, in->b, in->c, in->d, in->e, in->f};
      for (int k = 0; k < 6 && defc < 8; ++k)
        if (vals[k] >= 0)
          defs[defc++] = vals[k];
    }
    for (size_t k = 0; k < defc && topo_ok; ++k) {
      int d = defs[k];
      if (d < 0 || d >= f->next_value)
        continue;
      size_t def_block = (size_t)block_of_value[d];
      if (def_block == use_block)
        continue; /* intra-block: linear order inside the block is unchanged */
      if (!nyr_layout_blocks_are_ordered(def_block, use_block, order, blocks))
        topo_ok = false;
    }
  }
  free(block_of_value);

  bool changed = false;
  if (topo_ok) {
    /*
     * Detect whether the order actually differs from the current layout.
     */
    for (size_t i = 0; i < blocks && !changed; ++i)
      if (order[i] != i)
        changed = true;
  }

  if (changed) {
    /*
     * Rebuild the linear instruction stream in the new block order.  A
     * reordered `br.if` block whose fallthrough successor lands elsewhere in
     * the stream needs an explicit `br <fallthrough>` after it; count those
     * repairs first so the allocation is exact.
     */
    size_t body = 0;
    size_t extra = 0;
    for (size_t i = 0; i < blocks; ++i) {
      size_t b = order[i];
      body += cfg.block_end[b] - cfg.block_start[b];
      const nyr_inst_t *last = &f->data[cfg.block_end[b] - 1];
      if (last->op != NYIR_BR_IF)
        continue;
      size_t fallthrough = nyr_layout_fallthrough_succ(f, &cfg, b, last);
      if (i + 1 >= blocks || fallthrough == SIZE_MAX ||
          order[i + 1] != fallthrough)
        ++extra;
    }
    nyr_inst_t *reordered =
        ny_malloc_array(body + extra ? body + extra : 1, sizeof(*reordered));
    if (!reordered) {
      free(heat);
      free(done);
      free(order);
      nyr_cfg_free(&cfg);
      return false;
    }
    size_t w = 0;
    for (size_t i = 0; i < blocks; ++i) {
      size_t b = order[i];
      size_t start = cfg.block_start[b], end = cfg.block_end[b];
      for (size_t j = start; j < end; ++j)
        reordered[w++] = f->data[j];
      const nyr_inst_t *last = &f->data[end - 1];
      if (last->op != NYIR_BR_IF)
        continue;
      size_t fallthrough = nyr_layout_fallthrough_succ(f, &cfg, b, last);
      if (i + 1 < blocks && fallthrough != SIZE_MAX &&
          order[i + 1] == fallthrough)
        continue;
      if (fallthrough == SIZE_MAX)
        continue;
      int64_t ft_label = nyr_layout_block_label(f, &cfg, fallthrough);
      if (ft_label < 0)
        continue;
      nyr_inst_t br = {
          .op = NYIR_BR, .dst = -1, .a = -1, .b = -1, .imm = ft_label};
      br.effects = nyr_inst_effects(&br);
      reordered[w++] = br;
    }
    nyr_func_t candidate = {0};
    candidate = *f;
    candidate.data = reordered;
    candidate.len = w;
    candidate.cap = body + extra ? body + extra : 1;
    if (w >= f->len) {
      char err[256] = {0};
      if (nyr_ir_verify(&candidate, err, sizeof(err))) {
        free(f->data);
        f->data = candidate.data;
        f->len = candidate.len;
        f->cap = candidate.cap;
      } else {
        free(candidate.data);
      }
    } else {
      free(reordered);
    }
  }

  free(heat);
  free(done);
  nyr_inline_ool_flush("function");
  free(order);
  nyr_cfg_free(&cfg);
  return true;
}
