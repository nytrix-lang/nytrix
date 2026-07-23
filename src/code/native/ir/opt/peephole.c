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
    if (bk && bv == 0) nir_make_copy(in, in->a);
    break;
  case NYIR_MUL_I64:
    if ((bk && bv == 0) || (ak && av == 0)) nir_make_const(in, 0);
    else if (bk && bv == 1) nir_make_copy(in, in->a);
    else if (ak && av == 1) nir_make_copy(in, in->b);
    break;
  case NYIR_DIV_I64:
    if (bk && bv == 1) nir_make_copy(in, in->a);
    else if (ak && av == 0 && bk && bv != 0) nir_make_const(in, 0);
    break;
  case NYIR_MOD_I64:
    if (bk && (bv == 1 || bv == -1)) nir_make_const(in, 0);
    else if (ak && av == 0 && bk && bv != 0) nir_make_const(in, 0);
    break;
  case NYIR_AND_I64:
    if ((bk && bv == 0) || (ak && av == 0)) nir_make_const(in, 0);
    else if (bk && bv == -1) nir_make_copy(in, in->a);
    else if (ak && av == -1) nir_make_copy(in, in->b);
    break;
  case NYIR_OR_I64:
    if ((bk && bv == -1) || (ak && av == -1)) nir_make_const(in, -1);
    else if (bk && bv == 0) nir_make_copy(in, in->a);
    else if (ak && av == 0) nir_make_copy(in, in->b);
    break;
  case NYIR_XOR_I64:
    if (bk && bv == 0) nir_make_copy(in, in->a);
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
  nir_peephole_parallel_ctx_t parallel_ctx = {f, known, value};
  if (!ny_parallel_for(f->len, f->len, nir_peephole_parallel_task,
                       &parallel_ctx)) {
    free(known);
    free(value);
    free(facts);
    return false;
  }
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
        /* x * -1 == 0 - x under Nytrix's wrapping i64 mul. */
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
        /* x / -1 == 0 - x only when INT64_MIN is impossible (trap case). */
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
      if (bk && (bv == 1 || bv == -1))
        nir_make_const(in, 0);
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
      if (nir_operands_same_value(f, in->a, in->b, i))
        nir_make_copy(in, in->a);
      else if ((bk && bv == -1) || (ak && av == -1))
        nir_make_const(in, -1);
      else if (bk && bv == 0)
        nir_make_copy(in, in->a);
      else if (ak && av == 0)
        nir_make_copy(in, in->b);
      break;
    case NYIR_XOR_I64:
      if (nir_operands_same_value(f, in->a, in->b, i))
        nir_make_const(in, 0);
      else if (bk && bv == 0)
        nir_make_copy(in, in->a);
      else if (ak && av == 0)
        nir_make_copy(in, in->b);
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
    /* Float identity folds.  Constants are stored as int64 bitcasts. */
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
      break;
    /* Float32 identity folds. */
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
  free(known);
  free(value);
  free(facts);
  return true;
}
