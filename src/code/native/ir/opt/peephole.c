/*
 * Peephole optimizer: local instruction-sequence rewriting for
 * common instruction patterns (identity ops, redundant moves, etc.).
 */
#include "code/native/ir/opt/util.h"
#include "code/native/ir/internal.h"
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
    if (bk && bv == INT64_C(0)) nir_make_copy(in, in->a);
    else if (in->op == NYIR_ADD_F64 && ak && av == INT64_C(0)) nir_make_copy(in, in->b);
    break;
  case NYIR_MUL_F64:
    if ((bk && bv == INT64_C(0)) || (ak && av == INT64_C(0))) nir_make_f64_const(in, INT64_C(0));
    else if (bk && bv == INT64_C(4607182418800017408)) nir_make_copy(in, in->a);
    else if (ak && av == INT64_C(4607182418800017408)) nir_make_copy(in, in->b);
    break;
  case NYIR_DIV_F64:
    if (bk && bv == INT64_C(4607182418800017408)) nir_make_copy(in, in->a);
    break;
  case NYIR_ADD_F32:
  case NYIR_SUB_F32:
    if (bk && bv == 0) nir_make_copy(in, in->a);
    else if (in->op == NYIR_ADD_F32 && ak && av == 0) nir_make_copy(in, in->b);
    break;
  case NYIR_MUL_F32:
    if ((bk && bv == 0) || (ak && av == 0)) nir_make_f32_const(in, 0);
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
      break;
    case NYIR_SUB_I64:
      if (nir_operands_same_value(f, in->a, in->b, i))
        nir_make_const(in, 0);
      else if (bk && bv == 0)
        nir_make_copy(in, in->a);
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
      if (defs && in->a >= 0 && in->a < f->next_value && in->b >= 0 &&
          in->b < f->next_value) {
        int da_idx = defs[in->a];
        int db_idx = defs[in->b];
        if (da_idx >= 0 && (size_t)da_idx < f->len && db_idx >= 0 &&
            (size_t)db_idx < f->len) {
          const nyir_inst_t *da = &f->data[da_idx];
          const nyir_inst_t *db = &f->data[db_idx];
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
            if (shr_src == shl_src && shr_amt > 0 && shl_amt > 0 &&
                (shr_amt + shl_amt == 64 || shr_amt + shl_amt == 32)) {
              in->op = NYIR_ROR_I64;
              in->a = shr_src;
              in->b = shr->b;
              if (shr_amt + shl_amt == 32)
                in->flags |= NYIR_INST_F_NARROW32;
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
      if (bk && bv == NY_F64_BITCAST_0)
        nir_make_copy(in, in->a);
      else if (ak && av == NY_F64_BITCAST_0)
        nir_make_copy(in, in->b);
      break;
    case NYIR_SUB_F64:
      if (nir_operands_same_value(f, in->a, in->b, i))
        nir_make_f64_const(in, NY_F64_BITCAST_0);
      else if (bk && bv == NY_F64_BITCAST_0)
        nir_make_copy(in, in->a);
      break;
    case NYIR_MUL_F64:
      if ((bk && bv == NY_F64_BITCAST_0) || (ak && av == NY_F64_BITCAST_0))
        nir_make_f64_const(in, NY_F64_BITCAST_0);
      else if (bk && bv == NY_F64_BITCAST_1)
        nir_make_copy(in, in->a);
      else if (ak && av == NY_F64_BITCAST_1)
        nir_make_copy(in, in->b);
      break;
    case NYIR_DIV_F64:
      if (bk && bv == NY_F64_BITCAST_1)
        nir_make_copy(in, in->a);
      else if (ak && av == NY_F64_BITCAST_0 && bk && bv != NY_F64_BITCAST_0)
        nir_make_f64_const(in, NY_F64_BITCAST_0);
      else if (nir_operands_same_value(f, in->a, in->b, i))
        nir_make_f64_const(in, NY_F64_BITCAST_1);
      else if (bk && bv != NY_F64_BITCAST_0) {
        uint64_t ubits = (uint64_t)bv;
        uint64_t mantissa = ubits & UINT64_C(0x000FFFFFFFFFFFFF);
        uint64_t exp = (ubits >> 52) & 0x7FF;
        if (mantissa == 0 && exp > 0 && exp < 2046) {
          uint64_t inv_exp = 2046 - exp;
          uint64_t inv_bits = (ubits & (UINT64_C(1) << 63)) | (inv_exp << 52);
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
            int orig_dst = in->dst;
            int orig_a = in->a;
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
      if (bk && bv == NY_F32_BITCAST_0)
        nir_make_copy(in, in->a);
      else if (ak && av == NY_F32_BITCAST_0)
        nir_make_copy(in, in->b);
      break;
    case NYIR_SUB_F32:
      if (nir_operands_same_value(f, in->a, in->b, i))
        nir_make_f32_const(in, NY_F32_BITCAST_0);
      else if (bk && bv == NY_F32_BITCAST_0)
        nir_make_copy(in, in->a);
      break;
    case NYIR_MUL_F32:
      if ((bk && bv == NY_F32_BITCAST_0) || (ak && av == NY_F32_BITCAST_0))
        nir_make_f32_const(in, NY_F32_BITCAST_0);
      else if (bk && bv == NY_F32_BITCAST_1)
        nir_make_copy(in, in->a);
      else if (ak && av == NY_F32_BITCAST_1)
        nir_make_copy(in, in->b);
      break;
    case NYIR_DIV_F32:
      if (bk && bv == NY_F32_BITCAST_1)
        nir_make_copy(in, in->a);
      else if (ak && av == NY_F32_BITCAST_0 && bk && bv != NY_F32_BITCAST_0)
        nir_make_f32_const(in, NY_F32_BITCAST_0);
      else if (nir_operands_same_value(f, in->a, in->b, i))
        nir_make_f32_const(in, NY_F32_BITCAST_1);
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
   */
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

  return true;
}
