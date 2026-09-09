/*
 * Peephole optimizer: local instruction-sequence rewriting for
 * common patterns — algebraic identities, bitwise simplifications,
 * branchless selects, and two-level cancellations (e.g. (x+y)-y → x,
 * (x^y)^y → x, (x-y)+y → x). Run at O1+, re-run after IRCE.
 *
 * References:
 *  Warren — Hacker's Delight, 2nd Ed. (2012).
 *   Ch.2: bit manipulation identities, branchless abs/min/max/sign.
 *   Ch.10: division by constants (Granlund-Montgomery algorithm).
 *  Granlund & Montgomery — "Division by Invariant Integers using
 *   Multiplication" (PLDI 1994). §3 unsigned, §4 signed, §5 modulo.
 *  Bansal & Aiken — "Automatic Generation of Peephole Superoptimizers"
 *   (ASPLOS 2006). Exhaustive enumeration, Alive2-style validation.
 */
#include "code/ir/opt/util.h"
#include "code/ir/internal.h"
#include "base/compat.h"
#include "base/common.h"
#include "base/parallel.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


typedef struct {
  nyir_func_t *f;
  const bool *known;
  const int64_t *value;
  const nyir_value_fact_t *facts;
} nir_peephole_parallel_ctx_t;


/*
 * A def-map entry is only trusted while it still points at the instruction
 * that defines the value.  Inserting rewrites (nir_rewrite_shl/neg) shift
 * later indices and make cached entries stale; matching on a stale entry
 * turned algebraic identities into unsound cancellations (sha256 schedule
 * corruption).  Re-derive validity at each use instead.
 */
static const nyir_inst_t *nir_def_inst(const nyir_func_t *f, const int *defs,
                                        int value) {
  if (!defs || value < 0 || value >= f->next_value)
    return NULL;
  int idx = defs[value];
  if (idx < 0 || (size_t)idx >= f->len)
    return NULL;
  const nyir_inst_t *def = &f->data[idx];
  return def->dst == value ? def : NULL;
}

static bool nir_is_const_value(const nyir_func_t *f, const int *defs,
                               int value, int64_t expected) {
  const nyir_inst_t *def = nir_def_inst(f, defs, value);
  return def && def->op == NYIR_CONST_I64 && def->imm == expected;
}


static bool nir_is_ror32_count(const nyir_func_t *f, const int *defs,
                               const bool *known, const int64_t *value,
                               int count) {
  if (count < 0 || count >= f->next_value)
    return false;
  if (known[count])
    return value[count] >= 0 && value[count] < 32;
  const nyir_inst_t *def = nir_def_inst(f, defs, count);
  return def && def->op == NYIR_AND_I64 &&
         (nir_is_const_value(f, defs, def->a, 31) ||
          nir_is_const_value(f, defs, def->b, 31));
}

static bool nir_peephole_parallel_task(size_t i, void *opaque) {
  nir_peephole_parallel_ctx_t *ctx = (nir_peephole_parallel_ctx_t *)opaque;
  nyir_inst_t *in = &ctx->f->data[i];
  if (in->dst < 0 || in->a < 0 || in->b < 0)
    return true;
  bool ak = ctx->known[in->a];
  bool bk = ctx->known[in->b];
  int64_t av = ak ? ctx->value[in->a] : 0;
  int64_t bv = bk ? ctx->value[in->b] : 0;
  switch (in->op) {
  case NYIR_ADD_I64:
    if (bk && bv == 0) nir_make_copy(in, in->a);
    else if (ak && av == 0) nir_make_copy(in, in->b);
    break;
  case NYIR_SUB_I64:
    if (in->a == in->b) nir_make_const(in, 0);
    else if (bk && bv == 0) nir_make_copy(in, in->a);
    break;
  case NYIR_MUL_I64:
    if ((bk && bv == 0) || (ak && av == 0)) nir_make_const(in, 0);
    else if (bk && bv == 1) nir_make_copy(in, in->a);
    else if (ak && av == 1) nir_make_copy(in, in->b);
    break;
  case NYIR_DIV_I64: {
    if (bk && bv == 1) {
      nir_make_copy(in, in->a);
    } else if (ak && av == 0 && bk && bv != 0) {
      nir_make_const(in, 0);
    } else if (in->a == in->b) {
      nyir_range_t range = {0};
      if ((ak && av != 0) ||
          (ctx->facts && nir_value_range_at(ctx->f, ctx->facts, in->a, i, &range) &&
           nir_range_excludes_zero(&range)))
        nir_make_const(in, 1);
    }
    break;
  }
  case NYIR_MOD_I64: {
    if (bk && bv == 1) {
      nir_make_const(in, 0);
    } else if (bk && bv == -1) {
      nyir_range_t range = {0};
      bool safe = ak && av != INT64_MIN;
      if (!safe && ctx->facts &&
          nir_value_range_at(ctx->f, ctx->facts, in->a, i, &range))
        safe = nir_range_excludes_int64_min(&range);
      if (safe)
        nir_make_const(in, 0);
    } else if (ak && av == 0 && bk && bv != 0) {
      nir_make_const(in, 0);
    } else if (in->a == in->b) {
      nyir_range_t range = {0};
      if ((ak && av != 0) ||
          (ctx->facts && nir_value_range_at(ctx->f, ctx->facts, in->a, i, &range) &&
           nir_range_excludes_zero(&range)))
        nir_make_const(in, 0);
    }
    break;
  }
  case NYIR_AND_I64:
    if (in->a == in->b) nir_make_copy(in, in->a);
    else if ((bk && bv == 0) || (ak && av == 0)) nir_make_const(in, 0);
    else if (bk && bv == -1) nir_make_copy(in, in->a);
    else if (ak && av == -1) nir_make_copy(in, in->b);
    break;
  case NYIR_OR_I64:
    if (in->a == in->b) nir_make_copy(in, in->a);
    else if ((bk && bv == -1) || (ak && av == -1)) nir_make_const(in, -1);
    else if (bk && bv == 0) nir_make_copy(in, in->a);
    else if (ak && av == 0) nir_make_copy(in, in->b);
    break;
  case NYIR_XOR_I64:
    if (in->a == in->b) nir_make_const(in, 0);
    else if (bk && bv == 0) nir_make_copy(in, in->a);
    else if (ak && av == 0) nir_make_copy(in, in->b);
    break;
  case NYIR_SHL_I64:
  case NYIR_SAR_I64:
    if (bk && bv == 0) nir_make_copy(in, in->a);
    else if (ak && av == 0) nir_make_const(in, 0);
    break;
  case NYIR_ADD_F64:
  case NYIR_SUB_F64:
    /*
     * x - (+0.0) == x exactly, including x = -0.0/NaN/Inf. Adding
     * (+0.0) preserves x only when signed zeros agree (-0.0 + +0.0 is
     * +0.0), so the add forms are fast-math only.
     */
    if (in->op == NYIR_SUB_F64) {
      if (bk && bv == INT64_C(0)) nir_make_copy(in, in->a);
    } else if ((bk && bv == INT64_C(0)) || (ak && av == INT64_C(0))) {
      if (ny_native_fast_math()) nir_make_copy(in, bk ? in->a : in->b);
    }
    break;
  case NYIR_MUL_F64:
    /*
     * x * (+0.0) is +0.0 only for finite nonzero x; NaN/Inf/-signs differ.
     */
    if (((bk && bv == INT64_C(0)) || (ak && av == INT64_C(0))) &&
        ny_native_fast_math())
      nir_make_f64_const(in, INT64_C(0));
    else if (bk && bv == INT64_C(4607182418800017408)) nir_make_copy(in, in->a);
    else if (ak && av == INT64_C(4607182418800017408)) nir_make_copy(in, in->b);
    break;
  case NYIR_DIV_F64:
    if (bk && bv == INT64_C(4607182418800017408)) nir_make_copy(in, in->a);
    break;
  case NYIR_ADD_F32:
  case NYIR_SUB_F32:
    /*
     * See the f64 note: sub is exact, add is fast-math only.
     */
    if (in->op == NYIR_SUB_F32) {
      if (bk && bv == 0) nir_make_copy(in, in->a);
    } else if ((bk && bv == 0) || (ak && av == 0)) {
      if (ny_native_fast_math()) nir_make_copy(in, bk ? in->a : in->b);
    }
    break;
  case NYIR_MUL_F32:
    if (((bk && bv == 0) || (ak && av == 0)) && ny_native_fast_math())
      nir_make_f32_const(in, 0);
    else if (bk && bv == INT64_C(1065353216)) nir_make_copy(in, in->a);
    else if (ak && av == INT64_C(1065353216)) nir_make_copy(in, in->b);
    break;
  case NYIR_DIV_F32:
    if (bk && bv == INT64_C(1065353216)) nir_make_copy(in, in->a);
    break;
  default:
    break;
  }
  return true;
}

bool nyir_peephole(nyir_func_t *f) {
  if (!f || f->next_value <= 0)
    return true;
  bool *known = (bool *)calloc((size_t)f->next_value, sizeof(bool));
  int64_t *value = (int64_t *)calloc((size_t)f->next_value, sizeof(int64_t));
  nyir_value_fact_t *facts =
      (nyir_value_fact_t *)calloc((size_t)f->next_value, sizeof(*facts));
  if (!known || !value || !facts) {
    free(known);
    free(value);
    free(facts);
    return false;
  }
  if (!nir_collect_consts(f, known, value) ||
      !nyir_analyze_values(f, facts, (size_t)f->next_value, NULL, 0)) {
    free(known);
    free(value);
    free(facts);
    return false;
  }
  nir_peephole_parallel_ctx_t parallel_ctx = {f, known, value, facts};
  if (!ny_parallel_for(f->len, f->len, nir_peephole_parallel_task,
                       &parallel_ctx)) {
    free(known);
    free(value);
    free(facts);
    return false;
  }
  int *defs = nyir_build_defs(f);
  for (size_t i = 0; i < f->len; ++i) {
    nyir_inst_t *in = &f->data[i];
    if (in->dst < 0 || in->a < 0 || in->b < 0)
      continue;
    bool ak = known[in->a];
    bool bk = known[in->b];
    int64_t av = ak ? value[in->a] : 0;
    int64_t bv = bk ? value[in->b] : 0;
    switch (in->op) {
    case NYIR_ADD_I64:
      if (bk && bv == 0)
        nir_make_copy(in, in->a);
      else if (ak && av == 0)
        nir_make_copy(in, in->b);
      else if (defs && in->a >= 0 && in->a < f->next_value && in->b >= 0 && in->b < f->next_value) {
        const nyir_inst_t *da = nir_def_inst(f, defs, in->a);
        const nyir_inst_t *db = nir_def_inst(f, defs, in->b);
        if (da && da->op == NYIR_SUB_I64 && da->b == in->b) {
          nir_make_copy(in, da->a);
          break;
        }
        if (db && db->op == NYIR_SUB_I64 && db->b == in->a) {
          nir_make_copy(in, db->a);
          break;
        }
      }
      break;
    case NYIR_SUB_I64:
      if (nir_operands_same_value(f, in->a, in->b, i))
        nir_make_const(in, 0);
      else if (bk && bv == 0)
        nir_make_copy(in, in->a);
      else if (defs && in->a >= 0 && in->a < f->next_value && in->b >= 0 && in->b < f->next_value) {
        const nyir_inst_t *da = nir_def_inst(f, defs, in->a);
        const nyir_inst_t *db = nir_def_inst(f, defs, in->b);
        if (da && da->op == NYIR_ADD_I64) {
          if (da->a == in->b) { nir_make_copy(in, da->b); break; }
          if (da->b == in->b) { nir_make_copy(in, da->a); break; }
        }
        if (db && db->op == NYIR_SUB_I64 && db->a == in->a) {
          nir_make_copy(in, db->b);
          break;
        }
      }
      break;
    case NYIR_MUL_I64:
      if ((bk && bv == 0) || (ak && av == 0))
        nir_make_const(in, 0);
      else if (bk && bv == 1)
        nir_make_copy(in, in->a);
      else if (ak && av == 1)
        nir_make_copy(in, in->b);
      else if (bk && bv == -1) {
        /*
         * x * -1 == 0 - x under Nytrix's wrapping i64 mul.
         */
        if (!nir_rewrite_neg(f, &i, in->a)) {
          free(known);
          free(value);
          free(facts);
          return false;
        }
        continue;
      } else if (ak && av == -1) {
        if (!nir_rewrite_neg(f, &i, in->b)) {
          free(known);
          free(value);
          free(facts);
          return false;
        }
        continue;
      } else if (bk && bv > 1 && (bv & (bv - 1)) == 0) {
        /*
         * x * power_of_2 == x << log2(power_of_2).
         * Only for shift amounts in [1, 62] to stay within defined i64
         * shift behavior.
         */
        int shift = 0;
        int64_t v = bv;
        while (v > 1) { v >>= 1; shift++; }
        if (shift >= 1 && shift <= 62) {
          if (!nir_rewrite_shl(f, &i, in->a, shift)) {
            free(known);
            free(value);
            free(facts);
            return false;
          }
          continue;
        }
      } else if (ak && av > 1 && (av & (av - 1)) == 0) {
        int shift = 0;
        int64_t v = av;
        while (v > 1) { v >>= 1; shift++; }
        if (shift >= 1 && shift <= 62) {
          if (!nir_rewrite_shl(f, &i, in->b, shift)) {
            free(known);
            free(value);
            free(facts);
            return false;
          }
          continue;
        }
      }
      break;
    case NYIR_DIV_I64: {
      if (bk && bv == 1)
        nir_make_copy(in, in->a);
      else if (ak && av == 0 && bk && bv != 0)
        nir_make_const(in, 0);
      else if (nir_operands_same_value(f, in->a, in->b, i)) {
        nyir_range_t range = {0};
        if ((ak && av != 0) ||
            (nir_value_range_at(f, facts, in->a, i, &range) &&
             nir_range_excludes_zero(&range)))
          nir_make_const(in, 1);
      } else if (bk && bv == -1) {
        /*
         * x / -1 == 0 - x only when INT64_MIN is impossible (trap case).
         */
        nyir_range_t range = {0};
        bool safe = ak && av != INT64_MIN;
        if (!safe && nir_value_range_at(f, facts, in->a, i, &range))
          safe = nir_range_excludes_int64_min(&range);
        if (safe) {
          if (!nir_rewrite_neg(f, &i, in->a)) {
            free(known);
            free(value);
            free(facts);
            return false;
          }
          continue;
        }
      }
      break;
    }
    case NYIR_MOD_I64: {
      if (bk && bv == 1)
        nir_make_const(in, 0);
      else if (bk && bv == -1) {
        nyir_range_t range = {0};
        bool safe = ak && av != INT64_MIN;
        if (!safe && nir_value_range_at(f, facts, in->a, i, &range))
          safe = nir_range_excludes_int64_min(&range);
        if (safe)
          nir_make_const(in, 0);
      }
      else if (ak && av == 0 && bk && bv != 0)
        nir_make_const(in, 0);
      else if (nir_operands_same_value(f, in->a, in->b, i)) {
        nyir_range_t range = {0};
        if ((ak && av != 0) ||
            (nir_value_range_at(f, facts, in->a, i, &range) &&
             nir_range_excludes_zero(&range)))
          nir_make_const(in, 0);
      }
      break;
    }
    case NYIR_AND_I64:
      /*
       * A 32-bit rotate is sound only when both the input and complete result
       * are explicitly governed by the low-32 mask.  The count is either a
       * proven in-range constant or normalized with & 31; any partial or
       * cast-dependent shape remains ordinary i64 shifts and OR.
       */
      if (defs) {
        int or_value = -1;
        if (bk && bv == 0xffffffffLL)
          or_value = in->a;
        else if (ak && av == 0xffffffffLL)
          or_value = in->b;
        const nyir_inst_t *or_inst = nir_def_inst(f, defs, or_value);
        if (or_inst && or_inst->op == NYIR_OR_I64) {
          const nyir_inst_t *left = nir_def_inst(f, defs, or_inst->a);
          const nyir_inst_t *right = nir_def_inst(f, defs, or_inst->b);
          const nyir_inst_t *shr =
              left && left->op == NYIR_SAR_I64 ? left
              : right && right->op == NYIR_SAR_I64 ? right
                                                   : NULL;
          const nyir_inst_t *shl =
              left && left->op == NYIR_SHL_I64 ? left
              : right && right->op == NYIR_SHL_I64 ? right
                                                   : NULL;
          const nyir_inst_t *complement =
              shl ? nir_def_inst(f, defs, shl->b) : NULL;
          bool constant_complement =
              shr && shl && shr->b >= 0 && shl->b >= 0 &&
              known[shr->b] && known[shl->b] &&
              value[shr->b] >= 0 && value[shr->b] < 32 &&
              value[shl->b] == 32 - value[shr->b];
          bool symbolic_complement =
              complement && complement->op == NYIR_SUB_I64 &&
              nir_is_const_value(f, defs, complement->a, 32) &&
              complement->b == shr->b;
          if (shr && shl && shr->a == shl->a &&
              nir_is_ror32_count(f, defs, known, value, shr->b) &&
              (constant_complement || symbolic_complement)) {
            in->op = NYIR_ROR32_I64;
            in->a = shr->a;
            in->b = shr->b;
            in->flags &= ~NYIR_INST_F_NARROW32;
            in->effects = NYIR_EFFECT_NONE;
            break;
          }
        }
      }
      if (nir_operands_same_value(f, in->a, in->b, i))
        nir_make_copy(in, in->a);
      else if ((bk && bv == 0) || (ak && av == 0))
        nir_make_const(in, 0);
      else if (bk && bv == -1)
        nir_make_copy(in, in->a);
      else if (ak && av == -1)
        nir_make_copy(in, in->b);
      break;
    case NYIR_OR_I64:
    case NYIR_XOR_I64:
      if (nir_operands_same_value(f, in->a, in->b, i)) {
        if (in->op == NYIR_OR_I64)
          nir_make_copy(in, in->a);
        else
          nir_make_const(in, 0);
        break;
      }
      if (bk && bv == 0) {
        nir_make_copy(in, in->a);
        break;
      }
      if (ak && av == 0) {
        nir_make_copy(in, in->b);
        break;
      }
      if (in->op == NYIR_OR_I64 && ((bk && bv == -1) || (ak && av == -1))) {
        nir_make_const(in, -1);
        break;
      }
      if (in->op == NYIR_XOR_I64 && defs && in->a >= 0 &&
          in->a < f->next_value && in->b >= 0 && in->b < f->next_value) {
        const nyir_inst_t *da = nir_def_inst(f, defs, in->a);
        const nyir_inst_t *db = nir_def_inst(f, defs, in->b);
        if (da && da->op == NYIR_XOR_I64) {
          if (da->b == in->b) { nir_make_copy(in, da->a); break; }
          if (da->a == in->b) { nir_make_copy(in, da->b); break; }
        }
        if (db && db->op == NYIR_XOR_I64) {
          if (db->b == in->a) { nir_make_copy(in, db->a); break; }
          if (db->a == in->a) { nir_make_copy(in, db->b); break; }
        }
      }
      if (defs && in->a >= 0 && in->a < f->next_value && in->b >= 0 &&
          in->b < f->next_value) {
        const nyir_inst_t *da = nir_def_inst(f, defs, in->a);
        const nyir_inst_t *db = nir_def_inst(f, defs, in->b);
        if (da && db) {
          if ((da->op == NYIR_SAR_I64 && db->op == NYIR_SHL_I64) ||
              (da->op == NYIR_SHL_I64 && db->op == NYIR_SAR_I64)) {
            const nyir_inst_t *shr = da->op == NYIR_SAR_I64 ? da : db;
            const nyir_inst_t *shl = da->op == NYIR_SHL_I64 ? da : db;
            int shr_src = shr->a;
            int shl_src = shl->a;
            int64_t shr_amt =
                shr->b >= 0 && known[shr->b] ? value[shr->b] : -1;
            int64_t shl_amt =
                shl->b >= 0 && known[shl->b] ? value[shl->b] : -1;
            /*
             * Only fuse a full-width rotate: shr_amt + shl_amt must equal
             * the 64-bit register width.  A 32-bit pair (e.g. x>>7|x<<25)
             * leaves its high contributions above bit 31, which the
             * following & 0xFFFFFFFF relies on seeing — folding it into a
             * single rorq silently drops the wrapped bits and changes the
             * result (broke sha256's message schedule).
             *
             * SAR sign-fills, so (x SAR s) | (x SHL (64-s)) is only a true
             * rotate when x is provably non-negative; for negative sources
             * the OR cannot clear the propagated sign bits.
             */
            bool src_nonneg = false;
            if (shr_src >= 0 && shr_src < f->next_value) {
              nyir_range_t rr = facts[shr_src].range;
              if ((!rr.has_min || !rr.has_max) &&
                  nir_value_range_at(f, facts, shr_src, i, &rr))
                ;
              src_nonneg = rr.has_min && rr.min >= 0;
            }
            if (src_nonneg && shr_src == shl_src && shr_amt > 0 &&
                shl_amt > 0 && shr_amt + shl_amt == 64) {
              in->op = NYIR_ROR_I64;
              in->a = shr_src;
              in->b = shr->b;
              /*
               * No F_NARROW32 here: the verifier only allows that flag on
               * ADD/SUB/MUL/AND/OR/XOR/DIV/MOD, and no backend consumes it
               * for rotates — annotating made verify reject the function.
               */
              in->effects = NYIR_EFFECT_NONE;
              break;
            }
          }
        }
      }
      break;
    case NYIR_SHL_I64:
    case NYIR_SAR_I64:
      if (bk && bv == 0)
        nir_make_copy(in, in->a);
      else if (ak && av == 0)
        nir_make_const(in, 0);
      break;
    case NYIR_CMP_I64: {
      int64_t folded = 0;
      if (nir_operands_same_value(f, in->a, in->b, i)) {
        if (nir_cmp_same_value(in->cmp, &folded))
          nir_make_const(in, folded);
      } else if (in->a >= 0 && in->b >= 0) {
        nyir_range_t ra = {0};
        nyir_range_t rb = {0};
        const nyir_range_t *pa = &facts[in->a].range;
        const nyir_range_t *pb = &facts[in->b].range;
        if (!(pa->has_min || pa->has_max) &&
            nir_value_range_at(f, facts, in->a, i, &ra))
          pa = &ra;
        if (!(pb->has_min || pb->has_max) &&
            nir_value_range_at(f, facts, in->b, i, &rb))
          pb = &rb;
        if (nir_cmp_range_fold(in->cmp, pa, pb, &folded))
          nir_make_const(in, folded);
      }
      break;
    }
    /*
     * Float identity folds.  Constants are stored as int64 bitcasts.
     */
#define NY_F64_BITCAST_0   INT64_C(0)
#define NY_F64_BITCAST_1   INT64_C(4607182418800017408)
#define NY_F64_BITCAST_N1  INT64_C(-4616189618054758400)
    case NYIR_ADD_F64:
      /*
       * x + (+0.0) is x only for non-negative-zero x; fast-math only.
       */
      if (ny_native_fast_math() &&
          ((bk && bv == NY_F64_BITCAST_0) || (ak && av == NY_F64_BITCAST_0)))
        nir_make_copy(in, bk ? in->a : in->b);
      break;
    case NYIR_SUB_F64:
      /*
       * x - x is +0.0 only for finite x (NaN/Inf give NaN): fast-math.
       */
      if (nir_operands_same_value(f, in->a, in->b, i)) {
        if (ny_native_fast_math())
          nir_make_f64_const(in, NY_F64_BITCAST_0);
      } else if (bk && bv == NY_F64_BITCAST_0)
        nir_make_copy(in, in->a);
      break;
    case NYIR_MUL_F64:
      /*
       * x * (+0.0) is +0.0 only for finite nonzero x: fast-math.
       */
      if (((bk && bv == NY_F64_BITCAST_0) || (ak && av == NY_F64_BITCAST_0)) &&
          ny_native_fast_math())
        nir_make_f64_const(in, NY_F64_BITCAST_0);
      else if (bk && bv == NY_F64_BITCAST_1)
        nir_make_copy(in, in->a);
      else if (ak && av == NY_F64_BITCAST_1)
        nir_make_copy(in, in->b);
      break;
    case NYIR_DIV_F64:
      if (bk && bv == NY_F64_BITCAST_1)
        nir_make_copy(in, in->a);
      else if (ny_native_fast_math() && ak && av == NY_F64_BITCAST_0 &&
               bk && bv != NY_F64_BITCAST_0)
        nir_make_f64_const(in, NY_F64_BITCAST_0);
      else if (nir_operands_same_value(f, in->a, in->b, i)) {
        /*
         * 0/0 and Inf/Inf are NaN: fast-math only.
         */
        if (ny_native_fast_math())
          nir_make_f64_const(in, NY_F64_BITCAST_1);
      } else if (bk && bv != NY_F64_BITCAST_0) {
        if (!ny_native_fast_math())
          break;
        uint64_t ubits = (uint64_t)bv;
        uint64_t mantissa = ubits & UINT64_C(0x000FFFFFFFFFFFFF);
        uint64_t exp = (ubits >> 52) & 0x7FF;
        if (mantissa == 0 && exp > 0 && exp < 2046) {
          uint64_t inv_exp = 2046 - exp;
          uint64_t inv_bits = (ubits & (UINT64_C(1) << 63)) | (inv_exp << 52);
          /*
           * `in` points into f->data and may be invalidated by realloc.
           */
          int orig_dst = in->dst;
          int orig_a = in->a;
          int v_inv = f->next_value++;
          if (f->len + 1 > f->cap) {
            size_t new_cap = f->cap ? f->cap * 2 : 64;
            nyir_inst_t *new_data =
                (nyir_inst_t *)realloc(f->data, new_cap * sizeof(nyir_inst_t));
            if (new_data) {
              f->data = new_data;
              f->cap = new_cap;
            }
          }
          if (f->len + 1 <= f->cap) {
            memmove(&f->data[i + 1], &f->data[i],
                    (f->len - i) * sizeof(nyir_inst_t));
            f->len++;
            f->data[i] = (nyir_inst_t){.op = NYIR_CONST_F64,
                                       .dst = v_inv,
                                       .a = -1,
                                       .b = -1,
                                       .imm = (int64_t)inv_bits};
            f->data[i + 1] = (nyir_inst_t){.op = NYIR_MUL_F64,
                                           .dst = orig_dst,
                                           .a = orig_a,
                                           .b = v_inv,
                                           .effects = NYIR_EFFECT_NONE};
            i++;
            continue;
          }
        }
      }
      break;
    /*
     * Float32 identity folds.
     */
#define NY_F32_BITCAST_0   INT64_C(0)
#define NY_F32_BITCAST_1   INT64_C(1065353216)
    case NYIR_ADD_F32:
      /*
       * See the f64 note: fast-math only.
       */
      if (ny_native_fast_math() &&
          ((bk && bv == NY_F32_BITCAST_0) || (ak && av == NY_F32_BITCAST_0)))
        nir_make_copy(in, bk ? in->a : in->b);
      break;
    case NYIR_SUB_F32:
      if (nir_operands_same_value(f, in->a, in->b, i)) {
        if (ny_native_fast_math())
          nir_make_f32_const(in, NY_F32_BITCAST_0);
      } else if (bk && bv == NY_F32_BITCAST_0)
        nir_make_copy(in, in->a);
      break;
    case NYIR_MUL_F32:
      if (((bk && bv == NY_F32_BITCAST_0) || (ak && av == NY_F32_BITCAST_0)) &&
          ny_native_fast_math())
        nir_make_f32_const(in, NY_F32_BITCAST_0);
      else if (bk && bv == NY_F32_BITCAST_1)
        nir_make_copy(in, in->a);
      else if (ak && av == NY_F32_BITCAST_1)
        nir_make_copy(in, in->b);
      break;
    case NYIR_DIV_F32:
      if (bk && bv == NY_F32_BITCAST_1)
        nir_make_copy(in, in->a);
      else if (ny_native_fast_math() && ak && av == NY_F32_BITCAST_0 &&
               bk && bv != NY_F32_BITCAST_0)
        nir_make_f32_const(in, NY_F32_BITCAST_0);
      else if (nir_operands_same_value(f, in->a, in->b, i)) {
        if (ny_native_fast_math())
          nir_make_f32_const(in, NY_F32_BITCAST_1);
      }
      break;
#undef NY_F32_BITCAST_0
#undef NY_F32_BITCAST_1
#undef NY_F64_BITCAST_0
#undef NY_F64_BITCAST_1
#undef NY_F64_BITCAST_N1
    default:
      break;
    }
  }
  free(defs);
  free(known);
  free(value);
  free(facts);

  /*
   * Reciprocal division pass: transforms multiple divisions by the same
   * divisor within a basic block into one reciprocal division + multiplications.
   * fl(x * fl(1/d)) double-rounds, so this is only equivalent to x/d
   * under fast-math semantics.
   */
  if (ny_native_fast_math()) {
  for (size_t bb_start = 0; bb_start < f->len; ) {
    size_t bb_end = f->len;
    for (size_t j = bb_start; j < f->len; ++j) {
      nyir_op_t op = f->data[j].op;
      if (j > bb_start && (op == NYIR_LABEL || op == NYIR_BR ||
                           op == NYIR_BR_IF || op == NYIR_RET)) {
        bb_end = j;
        break;
      }
      if (op == NYIR_BR || op == NYIR_BR_IF || op == NYIR_RET) {
        bb_end = j + 1;
        break;
      }
    }

    for (size_t j = bb_start; j < bb_end; ++j) {
      if (f->data[j].op != NYIR_DIV_F64 || f->data[j].b < 0)
        continue;
      int divisor = f->data[j].b;

      size_t count = 0;
      for (size_t k = j; k < bb_end; ++k) {
        if (f->data[k].op == NYIR_DIV_F64 && f->data[k].b == divisor)
          count++;
      }

      if (count >= 2) {
        if (f->len + 2 > f->cap) {
          size_t new_cap = f->cap ? f->cap * 2 : 64;
          while (new_cap < f->len + 2)
            new_cap *= 2;
          nyir_inst_t *new_data =
              (nyir_inst_t *)realloc(f->data, new_cap * sizeof(nyir_inst_t));
          if (!new_data)
            return false;
          f->data = new_data;
          f->cap = new_cap;
        }

        int v_one = f->next_value++;
        int v_inv = f->next_value++;

        memmove(&f->data[j + 2], &f->data[j],
                (f->len - j) * sizeof(nyir_inst_t));
        f->len += 2;
        bb_end += 2;

        f->data[j] = (nyir_inst_t){.op = NYIR_CONST_F64,
                                   .dst = v_one,
                                   .a = -1,
                                   .b = -1,
                                   .imm = INT64_C(4607182418800017408)};
        f->data[j + 1] = (nyir_inst_t){.op = NYIR_DIV_F64,
                                       .dst = v_inv,
                                       .a = v_one,
                                       .b = divisor,
                                       .effects = NYIR_EFFECT_NONE};

        for (size_t k = j + 2; k < bb_end; ++k) {
          if (f->data[k].op == NYIR_DIV_F64 && f->data[k].b == divisor) {
            f->data[k].op = NYIR_MUL_F64;
            f->data[k].b = v_inv;
          }
        }
        j += 1;
      }
    }

    bb_start = bb_end;
  }
  }

  return true;
}
