/*
 * Overflow check elimination: removes proven-safe overflow guards.
 *
 * Literature
 *  Warren — Hacker's Delight (2nd Ed.), Ch.2 §2-13 "Overflow
 *   Detection": signed add/sub/mul overflow conditions via sign-
 *   bit tests. Unsigned via carry flag.
 *
 *  Dietz et al. — "Understanding Integer Overflow in C/C++"
 *   (ICSE 2012). Survey of compiler handling of signed overflow
 *   (UB) vs unsigned (wrap). §4: elimination strategies.
 *
 *  Nytrix VRP (vrp.c): provides range facts consumed by this pass.
 *
 * Nytrix implementation
 *  Eliminates overflow checks when VRP/SCCP prove the operation
 *  result stays within representable range. Tags proven operations
 *  with NYIR_INST_F_NO_OVERFLOW for codegen to skip checking.
 */
#include "base/debug.h"
#include "code/backend/passes/verify/loop.h"
#include "code/backend/passes/util.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Trace support for debugging overflow check elimination.
 */
static bool nyr_oce_trace_enabled(void) {
  return ny_dbg_is_enabled(NY_DBG_HEURISTIC) ||
         ny_trace_enabled("NY_TRACE_OCE");
}

static void nyr_oce_trace(const nyr_func_t *f, size_t at, const char *action,
                           const char *reason) {
  if (!nyr_oce_trace_enabled())
    return;
  const nyr_inst_t *in = &f->data[at];
  if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
    ny_dbg_heuristic_event("OVERFLOW_CHECK",
                           "%s @%zu (dst=%d op=%d a=%d b=%d): %s", action, at,
                           in->dst, in->op, in->a, in->b, reason);
  else
    ny_trace_line("OCE", "oce: %s @%zu (dst=%d op=%d a=%d b=%d): %s", action,
                  at, in->dst, in->op, in->a, in->b, reason);
}

/*
 * Check if a binary operation on two values is proven not to overflow.
 * Uses range facts from value analysis (SCCP) and SCEV loop-invariant bounds.
 */
static bool op_proven_no_overflow(const nyr_func_t *f,
                                  const nyr_value_fact_t *facts, nyr_op_t op,
                                  int a, int b) {
  if (!facts)
    return false;

  const nyr_range_t *ra =
      (a >= 0 && a < f->next_value) ? &facts[a].range : NULL;
  const nyr_range_t *rb =
      (b >= 0 && b < f->next_value) ? &facts[b].range : NULL;

  if (!ra || !rb || !ra->has_min || !ra->has_max || !rb->has_min ||
      !rb->has_max)
    return false;

  int64_t amin = ra->min, amax = ra->max;
  int64_t bmin = rb->min, bmax = rb->max;

  /*
   * Interval arithmetic: add/sub/mul are monotone in each operand, so the
   * extreme results sit at the box corners. Compute them in 128 bits;
   * every corner fitting int64 proves the whole interval does. This
   * replaces the previous per-quadrant hand proofs, several of which
   * paired the wrong corners (false NO_OVERFLOW) and divided
   * INT64_MIN / -1 in plain int64 (compiler SIGFPE).
   */
  switch (op) {
  case NYIR_ADD_I64: {
    ny_i128 hi = (ny_i128)amax + (ny_i128)bmax;
    ny_i128 lo = (ny_i128)amin + (ny_i128)bmin;
    if (lo >= INT64_MIN && hi <= INT64_MAX)
      return true;
    break;
  }

  case NYIR_SUB_I64: {
    ny_i128 hi = (ny_i128)amax - (ny_i128)bmin;
    ny_i128 lo = (ny_i128)amin - (ny_i128)bmax;
    if (lo >= INT64_MIN && hi <= INT64_MAX)
      return true;
    break;
  }

  case NYIR_MUL_I64: {
    ny_i128 p1 = (ny_i128)amin * (ny_i128)bmin;
    ny_i128 p2 = (ny_i128)amin * (ny_i128)bmax;
    ny_i128 p3 = (ny_i128)amax * (ny_i128)bmin;
    ny_i128 p4 = (ny_i128)amax * (ny_i128)bmax;
    ny_i128 hi = p1 > p2 ? p1 : p2;
    if (p3 > hi)
      hi = p3;
    if (p4 > hi)
      hi = p4;
    ny_i128 lo = p1 < p2 ? p1 : p2;
    if (p3 < lo)
      lo = p3;
    if (p4 < lo)
      lo = p4;
    if (lo >= INT64_MIN && hi <= INT64_MAX)
      return true;
    break;
  }

  case NYIR_SHL_I64:
    if (amin >= 0 && amax >= 0 && bmin >= 0 && bmax < 63) {
      if (amax <= (INT64_MAX >> (int)bmax))
        return true;
    }
    if (amax <= 0 && amin <= 0 && bmin >= 0 && bmax < 63) {
      if (amin >= (INT64_MIN >> (int)bmax))
        return true;
    }
    break;

  default:
    break;
  }
  return false;
}

/*
 * Check if a value is a known constant.
 */
static bool get_const_value(const nyr_func_t *f, const int *defs, int value,
                            int64_t *out) {
  if (value < 0 || value >= f->next_value)
    return false;
  int def = defs[value];
  if (def < 0)
    return false;
  const nyr_inst_t *in = &f->data[def];
  if (in->op == NYIR_CONST_I64) {
    *out = in->imm;
    return true;
  }
  return false;
}

/*
 * Eliminate overflow checks for operations with constant operands.
 */
static bool eliminate_const_overflow(nyr_func_t *f, const int *defs,
                                     size_t i) {
  nyr_inst_t *in = &f->data[i];

  if (in->op != NYIR_ADD_I64 && in->op != NYIR_SUB_I64 &&
      in->op != NYIR_MUL_I64 && in->op != NYIR_SHL_I64)
    return false;

  int64_t aval, bval;
  bool a_const = get_const_value(f, defs, in->a, &aval);
  bool b_const = get_const_value(f, defs, in->b, &bval);

  if (a_const && b_const) {
    ny_i128 result = 0;
    bool overflow = false;
    switch (in->op) {
    case NYIR_ADD_I64:
      result = (ny_i128)aval + bval;
      overflow = result < INT64_MIN || result > INT64_MAX;
      break;
    case NYIR_SUB_I64:
      result = (ny_i128)aval - bval;
      overflow = result < INT64_MIN || result > INT64_MAX;
      break;
    case NYIR_MUL_I64:
      result = (ny_i128)aval * bval;
      overflow = result < INT64_MIN || result > INT64_MAX;
      break;
    case NYIR_SHL_I64:
      if (bval < 0 || bval >= 64)
        overflow = true;
      else
        result = (ny_i128)aval << bval,
        overflow = result < INT64_MIN || result > INT64_MAX;
      break;
    default:
      return false;
    }
    if (!overflow) {
      nyr_oce_trace(f, i, "eliminate const",
                     "constant operation proven no overflow");
      return true;
    }
  }
  return false;
}

/*
 * Decompose `v` as base + offset through one constant ADD/SUB (and copies).
 * Used to recognise `x - 1` however constant folding spelled it.
 */
static void oce_base_offset(const nyr_func_t *f, const int *defs, int v,
                            int *base, int64_t *offset) {
  *base = v;
  *offset = 0;
  for (int depth = 0; depth < 4; ++depth) {
    if (*base < 0 || *base >= f->next_value || defs[*base] < 0)
      return;
    const nyr_inst_t *d = &f->data[(size_t)defs[*base]];
    int64_t c = 0;
    if (d->op == NYIR_COPY) {
      *base = d->a;
    } else if (d->op == NYIR_ADD_I64 && nyr_const_i64(f, defs, d->b, &c)) {
      *offset += c;
      *base = d->a;
    } else if (d->op == NYIR_ADD_I64 && nyr_const_i64(f, defs, d->a, &c)) {
      *offset += c;
      *base = d->b;
    } else if (d->op == NYIR_SUB_I64 && nyr_const_i64(f, defs, d->b, &c)) {
      *offset -= c;
      *base = d->a;
    } else {
      return;
    }
  }
}

/*
 * Main overflow check elimination pass.
 * Uses range facts from SCCP and SCEV loop-invariant bounds.
 */
bool nyr_overflow_check_elim(nyr_func_t *f) {
  if (!f || f->next_value <= 0)
    return true;

  int *defs = nyr_build_defs(f);
  nyr_value_fact_t *facts = calloc((size_t)f->next_value, sizeof(*facts));
  nyr_scev_info_t scev_info = {0};
  nyr_cfg_t cfg = {0};

  if (!defs || !facts || !nyr_cfg_build(f, &cfg) ||
      !nyr_analyze_values(f, facts, (size_t)f->next_value, NULL, 0)) {
    free(defs);
    free(facts);
    nyr_cfg_free(&cfg);
    return false;
  }

  /*
   * SCEV loop-invariant bounds feed nyr_analyze_values' range facts
   * indirectly; a direct in-loop query (the old calloc'd mask) is not
   * needed by the current proof rules.
   */
  (void)nyr_scev_analyze(f, &scev_info);

  for (size_t i = 0; i < f->len; ++i) {
    nyr_inst_t *in = &f->data[i];

    if (in->op == NYIR_ADD_I64 || in->op == NYIR_SUB_I64 ||
        in->op == NYIR_MUL_I64) {
      /*
       * F_NO_OVERFLOW is only legal on the verifier's allowed opcode set,
       * which excludes SHL: annotating shifts here would make the function
       * fail verification ("invalid instruction flags") even though nothing
       * consumes the flag on shifts today.
       */
      if (op_proven_no_overflow(f, facts, in->op, in->a, in->b)) {
        nyr_oce_trace(f, i, "mark no-overflow",
                       "range facts prove operation cannot overflow");
        in->flags |= NYIR_INST_F_NO_OVERFLOW;
        continue;
      }

      if (eliminate_const_overflow(f, defs, i)) {
        in->flags |= NYIR_INST_F_NO_OVERFLOW;
        continue;
      }
    }

    if ((in->op == NYIR_DIV_I64 || in->op == NYIR_MOD_I64) && in->a >= 0 &&
        in->a < f->next_value && in->b >= 0 && in->b < f->next_value) {
      const nyr_range_t *ra = &facts[in->a].range;
      const nyr_range_t *rb = &facts[in->b].range;
      if (ra->has_min && ra->has_max && ra->min >= 0 && ra->max <= INT32_MAX)
        in->flags |= NYIR_INST_F_DIVIDEND_U31;
      if (ra->has_min && ra->has_max && rb->has_min && rb->has_max) {
        /*
         * Division traps on zero divisors AND on INT64_MIN / -1.
         * Proving the divisor nonzero is not enough; the flag must also
         * exclude the MIN/-1 combination (verify.c legalizes
         * F_NO_OVERFLOW on DIV/MOD into non-checking codegen).
         */
        bool div0_safe = rb->min > 0 || rb->max < 0;
        bool min_minus_one = (ra->min <= INT64_MIN && INT64_MIN <= ra->max) &&
                             (rb->min <= -1 && -1 <= rb->max);
        if (div0_safe && !min_minus_one) {
          nyr_oce_trace(f, i, "eliminate div-trap",
                         "divisor excludes zero and MIN/-1 is unreachable");
          in->flags |= NYIR_INST_F_NO_OVERFLOW;
          continue;
        }
      }
    }
  }

  /*
   * Triangular-number halving. The induction-variable closed form computes
   * T*(T-1)/2 as (T>>1) * (T-1 + (T&1)) so the product never overflows. When
   * range facts bound T to [0, 2^31], T*(T-1) < 2^62 cannot wrap, and the
   * shorter (T*(T-1)) >> 1 is exact:
   *
   *   half = sar T, 1 ; bit = and T, 1 ; pf = add Tm1, bit ; r = mul half, pf
   *     ==>  half = mul T, Tm1 ; ... ; r = sar half, 1
   *
   * `half` keeps its position (and is used only by `r`), so Tm1 is already
   * defined there; `bit` and `pf` become dead.
   */
  {
    int *uses = calloc((size_t)f->next_value, sizeof(*uses));
    if (uses) {
      for (size_t i = 0; i < f->len; ++i) {
        const nyr_inst_t *u = &f->data[i];
        int ops[6] = {u->a, u->b, u->c, u->d, u->e, u->f};
        for (int k = 0; k < 6; ++k)
          if (ops[k] >= 0 && ops[k] < f->next_value)
            ++uses[ops[k]];
        for (size_t k = 0; k < u->phi_incoming_len; ++k) {
          int v = u->phi_incoming[k].value;
          if (v >= 0 && v < f->next_value)
            ++uses[v];
        }
        for (size_t k = 0; k < u->extra_args_len; ++k) {
          int v = u->extra_args[k];
          if (v >= 0 && v < f->next_value)
            ++uses[v];
        }
      }
      for (size_t i = 0; i < f->len; ++i) {
        nyr_inst_t *r = &f->data[i];
        if (r->op != NYIR_MUL_I64 || r->a < 0 || r->b < 0 ||
            r->a >= f->next_value || r->b >= f->next_value ||
            defs[r->a] < 0 || defs[r->b] < 0)
          continue;
        nyr_inst_t *half = &f->data[(size_t)defs[r->a]];
        nyr_inst_t *pf = &f->data[(size_t)defs[r->b]];
        if (half->op != NYIR_SAR_I64 || pf->op != NYIR_ADD_I64 ||
            uses[r->a] != 1 || (size_t)defs[r->a] >= i ||
            (size_t)defs[r->b] >= i)
          continue;
        int t = half->a;
        int one = half->b;
        int64_t shift = 0;
        if (t < 0 || t >= f->next_value || one < 0 || one >= f->next_value ||
            !nyr_const_i64(f, defs, one, &shift) || shift != 1)
          continue;
        const nyr_range_t *rt = &facts[t].range;
        if (!rt->has_min || !rt->has_max || rt->min < 0 ||
            rt->max > ((int64_t)1 << 31))
          continue;
        /*
         * pf = tm1 + (t & 1), tm1 == t - 1 (any spelling)
         */
        int tm1 = -1, bitv = -1;
        int t_base = 0;
        int64_t t_off = 0;
        oce_base_offset(f, defs, t, &t_base, &t_off);
        for (int side = 0; side < 2 && tm1 < 0; ++side) {
          int x = side ? pf->b : pf->a;
          int y = side ? pf->a : pf->b;
          if (x < 0 || y < 0 || x >= f->next_value || y >= f->next_value ||
              defs[x] < 0 || defs[y] < 0)
            continue;
          const nyr_inst_t *dy = &f->data[(size_t)defs[y]];
          int64_t c1 = 0;
          int x_base = 0;
          int64_t x_off = 0;
          oce_base_offset(f, defs, x, &x_base, &x_off);
          if (x_base == t_base && x_off == t_off - 1 &&
              dy->op == NYIR_AND_I64 &&
              ((dy->a == t && nyr_const_i64(f, defs, dy->b, &c1) && c1 == 1) ||
               (dy->b == t && nyr_const_i64(f, defs, dy->a, &c1) && c1 == 1))) {
            tm1 = x;
            bitv = y;
          }
        }
        if (tm1 < 0 || bitv < 0 || (size_t)defs[tm1] >= (size_t)defs[r->a])
          continue;
        nyr_oce_trace(f, i, "halve triangular product",
                      "T in [0,2^31]: (T*(T-1))>>1 is exact");
        half->op = NYIR_MUL_I64;
        half->b = tm1;
        half->flags |= NYIR_INST_F_NO_OVERFLOW;
        nyr_clear_derived_facts(half);
        r->op = NYIR_SAR_I64;
        r->b = one;
        r->flags &= ~(NYIR_INST_F_NARROW32 | NYIR_INST_F_NO_OVERFLOW);
        nyr_clear_derived_facts(r);
      }
      free(uses);
    }
  }

  free(defs);
  free(facts);
  nyr_cfg_free(&cfg);
  nyr_scev_free(&scev_info);
  /*
   * Pass contract: the boolean result reports success, not "changed".
   * Proving nothing new is a normal outcome.
   */
  return true;
}
