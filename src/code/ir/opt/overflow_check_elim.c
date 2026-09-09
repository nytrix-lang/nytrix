/*
 * Overflow check elimination: removes proven-safe overflow guards.
 *
 * ── Literature ──
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
 * ── Nytrix implementation ──
 *  Eliminates overflow checks when VRP/SCCP prove the operation
 *  result stays within representable range. Tags proven operations
 *  with NYIR_INST_F_NO_OVERFLOW for codegen to skip checking.
 */
#include "code/ir/opt/util.h"
#include "code/ir/opt/loop.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

/*
 * Trace support for debugging overflow check elimination.
 */
static bool nyir_oce_trace_enabled(void) {
  return ny_trace_enabled("NY_TRACE_OCE");
}

static void nyir_oce_trace(const nyir_func_t *f, size_t at, const char *action,
                           const char *reason) {
  if (!nyir_oce_trace_enabled())
    return;
  const nyir_inst_t *in = &f->data[at];
  ny_trace_line("OCE", "oce: %s @%zu (dst=%d op=%d a=%d b=%d): %s",
                action, at, in->dst, in->op, in->a, in->b, reason);
}

/*
 * Check if a binary operation on two values is proven not to overflow.
 * Uses range facts from value analysis (SCCP) and SCEV loop-invariant bounds.
 */
static bool op_proven_no_overflow(const nyir_func_t *f,
                                  const nyir_value_fact_t *facts,
                                  nyir_op_t op, int a, int b) {
  if (!facts)
    return false;

  const nyir_range_t *ra = (a >= 0 && a < f->next_value) ? &facts[a].range : NULL;
  const nyir_range_t *rb = (b >= 0 && b < f->next_value) ? &facts[b].range : NULL;

  if (!ra || !rb || !ra->has_min || !ra->has_max || !rb->has_min || !rb->has_max)
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
    __int128 hi = (__int128)amax + (__int128)bmax;
    __int128 lo = (__int128)amin + (__int128)bmin;
    if (lo >= INT64_MIN && hi <= INT64_MAX)
      return true;
    break;
  }

  case NYIR_SUB_I64: {
    __int128 hi = (__int128)amax - (__int128)bmin;
    __int128 lo = (__int128)amin - (__int128)bmax;
    if (lo >= INT64_MIN && hi <= INT64_MAX)
      return true;
    break;
  }

  case NYIR_MUL_I64: {
    __int128 p1 = (__int128)amin * (__int128)bmin;
    __int128 p2 = (__int128)amin * (__int128)bmax;
    __int128 p3 = (__int128)amax * (__int128)bmin;
    __int128 p4 = (__int128)amax * (__int128)bmax;
    __int128 hi = p1 > p2 ? p1 : p2;
    if (p3 > hi) hi = p3;
    if (p4 > hi) hi = p4;
    __int128 lo = p1 < p2 ? p1 : p2;
    if (p3 < lo) lo = p3;
    if (p4 < lo) lo = p4;
    if (lo >= INT64_MIN && hi <= INT64_MAX)
      return true;
    break;
  }

  case NYIR_SHL_I64:
    if (amin >= 0 && amax >= 0 && bmin >= 0 && bmax < 63) {
      if (amax <= (INT64_MAX >> (int)bmax)) return true;
    }
    if (amax <= 0 && amin <= 0 && bmin >= 0 && bmax < 63) {
      if (amin >= (INT64_MIN >> (int)bmax)) return true;
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
static bool get_const_value(const nyir_func_t *f, const int *defs,
                            int value, int64_t *out) {
  if (value < 0 || value >= f->next_value)
    return false;
  int def = defs[value];
  if (def < 0)
    return false;
  const nyir_inst_t *in = &f->data[def];
  if (in->op == NYIR_CONST_I64) {
    *out = in->imm;
    return true;
  }
  return false;
}

/*
 * Eliminate overflow checks for operations with constant operands.
 */
static bool eliminate_const_overflow(nyir_func_t *f, const int *defs,
                                     size_t i) {
  nyir_inst_t *in = &f->data[i];

  if (in->op != NYIR_ADD_I64 && in->op != NYIR_SUB_I64 &&
      in->op != NYIR_MUL_I64 && in->op != NYIR_SHL_I64)
    return false;

  int64_t aval, bval;
  bool a_const = get_const_value(f, defs, in->a, &aval);
  bool b_const = get_const_value(f, defs, in->b, &bval);

  if (a_const && b_const) {
    __int128 result = 0;
    bool overflow = false;
    switch (in->op) {
    case NYIR_ADD_I64:
      result = (__int128)aval + bval;
      overflow = result < INT64_MIN || result > INT64_MAX;
      break;
    case NYIR_SUB_I64:
      result = (__int128)aval - bval;
      overflow = result < INT64_MIN || result > INT64_MAX;
      break;
    case NYIR_MUL_I64:
      result = (__int128)aval * bval;
      overflow = result < INT64_MIN || result > INT64_MAX;
      break;
    case NYIR_SHL_I64:
      if (bval < 0 || bval >= 64)
        overflow = true;
      else
        result = (__int128)aval << bval, overflow = result < INT64_MIN || result > INT64_MAX;
      break;
    default:
      return false;
    }
    if (!overflow) {
      nyir_oce_trace(f, i, "eliminate const",
                     "constant operation proven no overflow");
      return true;
    }
  }
  return false;
}

/*
 * Main overflow check elimination pass.
 * Uses range facts from SCCP and SCEV loop-invariant bounds.
 */
bool nyir_overflow_check_elim(nyir_func_t *f) {
  if (!f || f->next_value <= 0)
    return true;

  int *defs = nyir_build_defs(f);
  nyir_value_fact_t *facts =
      calloc((size_t)f->next_value, sizeof(*facts));
  nyir_scev_info_t scev_info = {0};
  nyir_cfg_t cfg = {0};

  if (!defs || !facts || !nyir_cfg_build(f, &cfg) ||
      !nyir_analyze_values(f, facts, (size_t)f->next_value, NULL, 0)) {
    free(defs);
    free(facts);
    nyir_cfg_free(&cfg);
    return false;
  }

  /*
   * SCEV loop-invariant bounds feed nyir_analyze_values' range facts
   * indirectly; a direct in-loop query (the old calloc'd mask) is not
   * needed by the current proof rules.
   */
  (void)nyir_scev_analyze(f, &scev_info);

  for (size_t i = 0; i < f->len; ++i) {
    nyir_inst_t *in = &f->data[i];

    if (in->op == NYIR_ADD_I64 || in->op == NYIR_SUB_I64 ||
        in->op == NYIR_MUL_I64) {
      /*
       * F_NO_OVERFLOW is only legal on the verifier's allowed opcode set,
       * which excludes SHL: annotating shifts here would make the function
       * fail verification ("invalid instruction flags") even though nothing
       * consumes the flag on shifts today.
       */
      if (op_proven_no_overflow(f, facts, in->op, in->a, in->b)) {
        nyir_oce_trace(f, i, "mark no-overflow",
                       "range facts prove operation cannot overflow");
        in->flags |= NYIR_INST_F_NO_OVERFLOW;
        continue;
      }

      if (eliminate_const_overflow(f, defs, i)) {
        in->flags |= NYIR_INST_F_NO_OVERFLOW;
        continue;
      }
    }

    if ((in->op == NYIR_DIV_I64 || in->op == NYIR_MOD_I64) &&
        in->a >= 0 && in->a < f->next_value &&
        in->b >= 0 && in->b < f->next_value) {
      const nyir_range_t *ra = &facts[in->a].range;
      const nyir_range_t *rb = &facts[in->b].range;
      if (ra->has_min && ra->has_max && rb->has_min && rb->has_max) {
        /*
         * Division traps on zero divisors AND on INT64_MIN / -1.
         * Proving the divisor nonzero is not enough; the flag must also
         * exclude the MIN/-1 combination (verify.c legalizes
         * F_NO_OVERFLOW on DIV/MOD into non-checking codegen).
         */
        bool div0_safe = rb->min > 0 || rb->max < 0;
        bool min_minus_one =
            (ra->min <= INT64_MIN && INT64_MIN <= ra->max) &&
            (rb->min <= -1 && -1 <= rb->max);
        if (div0_safe && !min_minus_one) {
          nyir_oce_trace(f, i, "eliminate div-trap",
                         "divisor excludes zero and MIN/-1 is unreachable");
          in->flags |= NYIR_INST_F_NO_OVERFLOW;
          continue;
        }
      }
    }
  }

  free(defs);
  free(facts);
  nyir_cfg_free(&cfg);
  nyir_scev_free(&scev_info);
  /*
   * Pass contract: the boolean result reports success, not "changed".
   * Proving nothing new is a normal outcome.
   */
  return true;
}