/*
 * VRP: value range propagation through SSA.
 *
 * Literature
 *  Patterson — "Accurate Static Branch Prediction by Value Range
 *   Propagation" (PLDI 1995). Interval lattice with forward/
 *   backward propagation. §3 transfer functions; §4 narrowing.
 *
 *  GCC tree-vrp.c: range lattice [min,max] feeding branch
 *   prediction, BCE, and overflow elimination.
 *
 * Nytrix implementation
 *  Forward CFG propagation with narrowing at conditionals.
 *  Consumed by IRCE, BCE, overflow/null check elimination.
 */
#include "code/backend/passes/util.h"
#include "code/backend/passes/verify/loop.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  int64_t min;
  int64_t max;
  bool has_min;
  bool has_max;
} vrp_range_t;

/*
 * Unknown is the top element: no bound is stronger than no fact.
 */
static void vrp_range_full(vrp_range_t *r) {
  if (r)
    *r = (vrp_range_t){0};
}

/*
 * Set range to a constant.
 */
static void vrp_range_const(vrp_range_t *r, int64_t v) {
  r->has_min = true;
  r->has_max = true;
  r->min = v;
  r->max = v;
}

/*
 * Compute range for binary operation.
 */
static void vrp_compute_binop(vrp_range_t *dst, nyr_op_t op,
                              const vrp_range_t *a, const vrp_range_t *b) {
  vrp_range_full(dst);
  if (!a->has_min || !a->has_max || !b->has_min || !b->has_max)
    return;

  int64_t amin = a->min, amax = a->max;
  int64_t bmin = b->min, bmax = b->max;
  ny_i128 rmin, rmax;

  switch (op) {
  case NYIR_ADD_I64:
    rmin = (ny_i128)amin + bmin;
    rmax = (ny_i128)amax + bmax;
    break;
  case NYIR_SUB_I64:
    rmin = (ny_i128)amin - bmax;
    rmax = (ny_i128)amax - bmin;
    break;
  case NYIR_MUL_I64: {
    ny_i128 vals[4] = {(ny_i128)amin * bmin, (ny_i128)amin * bmax,
                        (ny_i128)amax * bmin, (ny_i128)amax * bmax};
    rmin = vals[0];
    rmax = vals[0];
    for (int i = 1; i < 4; ++i) {
      if (vals[i] < rmin)
        rmin = vals[i];
      if (vals[i] > rmax)
        rmax = vals[i];
    }
    break;
  }
  case NYIR_AND_I64:
    /*
     * For non-negative operands the unsigned bit lattice gives a sound
     * signed bound.  Signed operands can produce any negative bit pattern.
     */
    if (amin < 0 || bmin < 0)
      return;
    rmin = 0;
    rmax = amax < bmax ? amax : bmax;
    break;
  case NYIR_OR_I64:
  case NYIR_XOR_I64: {
    if (amin < 0 || bmin < 0)
      return;
    uint64_t mask = (uint64_t)amax | (uint64_t)bmax;
    mask |= mask >> 1;
    mask |= mask >> 2;
    mask |= mask >> 4;
    mask |= mask >> 8;
    mask |= mask >> 16;
    mask |= mask >> 32;
    if (mask > (uint64_t)INT64_MAX)
      return;
    rmin = 0;
    rmax = (ny_i128)mask;
    break;
  }
  case NYIR_SHL_I64:
    if (bmin < 0 || bmax >= 63)
      return;
    if (bmin == bmax) {
      ny_i128 scale = ((ny_i128)1) << (int)bmin;
      rmin = (ny_i128)amin * scale;
      rmax = (ny_i128)amax * scale;
    } else {
      if (amin < 0)
        return;
      rmin = (ny_i128)amin << (int)bmin;
      rmax = (ny_i128)amax << (int)bmax;
    }
    break;
  case NYIR_SAR_I64: {
    if (bmin < 0 || bmax >= 64)
      return;
    int64_t vals[4] = {amin >> (int)bmin, amin >> (int)bmax, amax >> (int)bmin,
                       amax >> (int)bmax};
    rmin = vals[0];
    rmax = vals[0];
    for (int i = 1; i < 4; ++i) {
      if (vals[i] < rmin)
        rmin = vals[i];
      if (vals[i] > rmax)
        rmax = vals[i];
    }
    break;
  }
  case NYIR_DIV_I64: {
    if (bmin <= 0 && bmax >= 0)
      return;
    int64_t av[2] = {amin, amax};
    int64_t bv[2] = {bmin, bmax};
    ny_i128 vals[4];
    size_t n = 0;
    for (size_t i = 0; i < 2; ++i)
      for (size_t j = 0; j < 2; ++j) {
        if (av[i] == INT64_MIN && bv[j] == -1)
          return;
        vals[n++] = av[i] / bv[j];
      }
    rmin = vals[0];
    rmax = vals[0];
    for (size_t i = 1; i < n; ++i) {
      if (vals[i] < rmin)
        rmin = vals[i];
      if (vals[i] > rmax)
        rmax = vals[i];
    }
    break;
  }
  case NYIR_MOD_I64: {
    if (bmin <= 0 && bmax >= 0)
      return;
    int64_t magnitude = INT64_MAX;
    if (bmin != INT64_MIN && bmax != INT64_MIN) {
      int64_t x = bmin < 0 ? -bmin : bmin;
      int64_t y = bmax < 0 ? -bmax : bmax;
      magnitude = (x > y ? x : y) - 1;
    }
    rmin = amax <= 0   ? -(ny_i128)magnitude
           : amin >= 0 ? 0
                       : -(ny_i128)magnitude;
    rmax = amin >= 0 ? magnitude : amax <= 0 ? 0 : magnitude;
    break;
  }
  default:
    return;
  }

  if (rmin < INT64_MIN || rmax > INT64_MAX || rmin > rmax)
    return;
  dst->has_min = true;
  dst->has_max = true;
  dst->min = (int64_t)rmin;
  dst->max = (int64_t)rmax;
}

/*
 * Comparisons are always boolean; disjoint intervals make them constants.
 */
static void vrp_compute_cmp(vrp_range_t *dst, nyr_cmp_t cmp,
                            const vrp_range_t *a, const vrp_range_t *b) {
  bool known = false, result = false;
  vrp_range_const(dst, 0);
  dst->max = 1;
  if (!a->has_min || !a->has_max || !b->has_min || !b->has_max)
    return;
  switch (cmp) {
  case NYIR_CMP_EQ:
    known = a->max < b->min || b->max < a->min ||
            (a->min == a->max && b->min == b->max);
    result = a->min == a->max && b->min == b->max && a->min == b->min;
    break;
  case NYIR_CMP_NE:
    known = a->max < b->min || b->max < a->min ||
            (a->min == a->max && b->min == b->max);
    result = a->max < b->min || b->max < a->min ||
             (a->min == a->max && b->min == b->max && a->min != b->min);
    break;
  case NYIR_CMP_LT:
    known = a->max < b->min || a->min >= b->max;
    result = a->max < b->min;
    break;
  case NYIR_CMP_LE:
    known = a->max <= b->min || a->min > b->max;
    result = a->max <= b->min;
    break;
  case NYIR_CMP_GT:
    known = a->min > b->max || a->max <= b->min;
    result = a->min > b->max;
    break;
  case NYIR_CMP_GE:
    known = a->min >= b->max || a->max < b->min;
    result = a->min >= b->max;
    break;
  }
  if (known)
    vrp_range_const(dst, result ? 1 : 0);
}

/*
 * Compute range for PHI node.
 */
static void vrp_compute_phi(vrp_range_t *dst, size_t incoming_count,
                            const vrp_range_t *incoming) {
  vrp_range_full(dst);
  if (!incoming || incoming_count == 0)
    return;
  for (size_t i = 0; i < incoming_count; ++i) {
    if (!incoming[i].has_min || !incoming[i].has_max) {
      vrp_range_full(dst);
      return;
    }
    if (!dst->has_min || incoming[i].min < dst->min)
      dst->min = incoming[i].min;
    if (!dst->has_max || incoming[i].max > dst->max)
      dst->max = incoming[i].max;
    dst->has_min = true;
    dst->has_max = true;
  }
}

/*
 * Main Value Range Propagation pass.
 * Computes ranges for all SSA values using forward dataflow analysis.
 */
typedef struct {
  bool valid;
  size_t loop_idx;
  int64_t init;
  int64_t step;
} vrp_affine_t;

static int vrp_resolve_copy(const nyr_func_t *f, const int *defs,
                            size_t num_values, int v) {
  for (unsigned steps = 0; steps < 8; ++steps) {
    if (v < 0 || (size_t)v >= num_values || defs[v] < 0)
      return v;
    const nyr_inst_t *in = &f->data[(size_t)defs[v]];
    if (in->op != NYIR_COPY)
      return v;
    v = in->a;
  }
  return v;
}

static bool vrp_const_i64(const nyr_func_t *f, const int *defs,
                          size_t num_values, int v, int64_t *out) {
  v = vrp_resolve_copy(f, defs, num_values, v);
  if (v < 0 || (size_t)v >= num_values || defs[v] < 0)
    return false;
  const nyr_inst_t *in = &f->data[(size_t)defs[v]];
  if (in->op != NYIR_CONST_I64)
    return false;
  *out = in->imm;
  return true;
}

static void vrp_seed_affine_bounds(vrp_range_t *dst, int64_t init,
                                   int64_t step, uint64_t trip_count) {
  if (trip_count > (uint64_t)INT64_MAX)
    return;
  ny_i128 lo = init;
  ny_i128 hi = (ny_i128)init + (ny_i128)trip_count * (ny_i128)step;
  if (lo > hi) {
    ny_i128 tmp = lo;
    lo = hi;
    hi = tmp;
  }
  if (lo < INT64_MIN || hi > INT64_MAX)
    return;
  dst->has_min = true;
  dst->has_max = true;
  dst->min = (int64_t)lo;
  dst->max = (int64_t)hi;
}

static bool vrp_sub_self_shr(const nyr_func_t *f, const int *defs,
                             size_t num_values, int lhs, int rhs) {
  int a = vrp_resolve_copy(f, defs, num_values, lhs);
  int b = vrp_resolve_copy(f, defs, num_values, rhs);
  if (b < 0 || (size_t)b >= num_values || defs[b] < 0)
    return false;
  const nyr_inst_t *bin = &f->data[(size_t)defs[b]];
  if (bin->op == NYIR_AND_I64) {
    int64_t m = 0;
    int inner = -1;
    if (vrp_const_i64(f, defs, num_values, bin->b, &m) && m >= 0)
      inner = bin->a;
    else if (vrp_const_i64(f, defs, num_values, bin->a, &m) && m >= 0)
      inner = bin->b;
    else
      return false;
    b = vrp_resolve_copy(f, defs, num_values, inner);
    if (b < 0 || (size_t)b >= num_values || defs[b] < 0)
      return false;
    bin = &f->data[(size_t)defs[b]];
  }
  int64_t sh = 0;
  return bin->op == NYIR_SAR_I64 &&
         vrp_const_i64(f, defs, num_values, bin->b, &sh) && sh >= 1 &&
         sh <= 62 && vrp_resolve_copy(f, defs, num_values, bin->a) == a;
}

bool nyr_vrp(nyr_func_t *f) {
  if (!f || f->next_value <= 0)
    return true;

  int *defs = nyr_build_defs(f);
  nyr_cfg_t cfg = {0};
  nyr_scev_info_t scev = {0};
  if (!defs || !nyr_cfg_build(f, &cfg) || !nyr_scev_analyze(f, &scev)) {
    free(defs);
    nyr_cfg_free(&cfg);
    nyr_scev_free(&scev);
    return false;
  }

  size_t num_values = (size_t)f->next_value;
  vrp_range_t *ranges = calloc(num_values, sizeof(*ranges));
  vrp_range_t *new_ranges = calloc(num_values, sizeof(*new_ranges));
  vrp_affine_t *affines = calloc(num_values, sizeof(*affines));
  bool *scev_seeded = calloc(num_values, sizeof(*scev_seeded));
  bool *in_worklist = calloc(cfg.block_count, sizeof(*in_worklist));
  size_t *worklist = calloc(cfg.block_count, sizeof(*worklist));
  size_t wl_head = 0, wl_tail = 0;

  if (!ranges || !new_ranges || !affines || !scev_seeded || !in_worklist ||
      !worklist) {
    free(ranges);
    free(new_ranges);
    free(affines);
    free(scev_seeded);
    free(in_worklist);
    free(worklist);
    free(defs);
    nyr_cfg_free(&cfg);
    nyr_scev_free(&scev);
    return false;
  }

  /*
   * Initialize: constants have known ranges.
   */
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->op == NYIR_CONST_I64 && in->dst >= 0 &&
        (size_t)in->dst < num_values) {
      vrp_range_const(&ranges[in->dst], in->imm);
    }
  }

  /*
   * Seed loop-header induction PHIs from SCEV trip proofs so derived linear
   * recurrences (including secondary stride PHIs inserted by strength
   * reduction / IV elim) preserve tight bounds through the loop.
   */
  for (size_t li = 0; li < scev.count; ++li) {
    const nyr_scev_loop_t *loop = &scev.loops[li];
    if (!loop->trip_count_known && !loop->iv_bounds_known)
      continue;
    if (loop->iv >= 0 && (size_t)loop->iv < num_values) {
      if (loop->iv_bounds_known) {
        ranges[loop->iv] = (vrp_range_t){.min = loop->iv_min,
                                         .max = loop->iv_max,
                                         .has_min = true,
                                         .has_max = true};
        scev_seeded[loop->iv] = true;
      } else if (loop->init_is_const) {
        vrp_seed_affine_bounds(&ranges[loop->iv], loop->init, loop->step,
                               loop->trip_count);
        if (ranges[loop->iv].has_min && ranges[loop->iv].has_max)
          scev_seeded[loop->iv] = true;
      }
      if (loop->trip_count_known && loop->init_is_const)
        affines[loop->iv] = (vrp_affine_t){.valid = true,
                                           .loop_idx = li,
                                           .init = loop->init,
                                           .step = loop->step};
    }
    if (!loop->trip_count_known || loop->header_block >= cfg.block_count ||
        loop->latch_block >= cfg.block_count)
      continue;
    int64_t latch_lbl = cfg.block_label[loop->latch_block];
    for (size_t i = cfg.block_start[loop->header_block];
         i < cfg.block_end[loop->header_block]; ++i) {
      const nyr_inst_t *phi = &f->data[i];
      if (phi->op != NYIR_PHI || phi->dst < 0 ||
          (size_t)phi->dst >= num_values || phi->dst == loop->iv ||
          phi->phi_incoming_len != 2 || !phi->phi_incoming)
        continue;
      int pre_v = -1, back_v = -1;
      for (size_t k = 0; k < phi->phi_incoming_len; ++k) {
        if (phi->phi_incoming[k].predecessor_label == latch_lbl)
          back_v = phi->phi_incoming[k].value;
        else
          pre_v = phi->phi_incoming[k].value;
      }
      int64_t init_c = 0;
      if (!vrp_const_i64(f, defs, num_values, pre_v, &init_c) || back_v < 0 ||
          (size_t)back_v >= num_values || defs[back_v] < 0)
        continue;
      const nyr_inst_t *step_in = &f->data[(size_t)defs[back_v]];
      int64_t step_c = 0;
      bool step_ok = false;
      if (step_in->op == NYIR_ADD_I64) {
        if (vrp_resolve_copy(f, defs, num_values, step_in->a) == phi->dst &&
            vrp_const_i64(f, defs, num_values, step_in->b, &step_c))
          step_ok = true;
        else if (vrp_resolve_copy(f, defs, num_values, step_in->b) ==
                     phi->dst &&
                 vrp_const_i64(f, defs, num_values, step_in->a, &step_c))
          step_ok = true;
      } else if (step_in->op == NYIR_SUB_I64 &&
                 vrp_resolve_copy(f, defs, num_values, step_in->a) ==
                     phi->dst &&
                 vrp_const_i64(f, defs, num_values, step_in->b, &step_c) &&
                 step_c != INT64_MIN) {
        step_c = -step_c;
        step_ok = true;
      }
      if (!step_ok)
        continue;
      vrp_range_t r = {0};
      vrp_seed_affine_bounds(&r, init_c, step_c, loop->trip_count);
      if (r.has_min && r.has_max) {
        ranges[phi->dst] = r;
        scev_seeded[phi->dst] = true;
        affines[phi->dst] = (vrp_affine_t){
            .valid = true, .loop_idx = li, .init = init_c, .step = step_c};
      }
    }
  }

  /*
   * Initialize worklist with all blocks.
   */
  for (size_t b = 0; b < cfg.block_count; ++b) {
    worklist[wl_tail++] = b;
    in_worklist[b] = true;
  }

  /*
   * Worklist is a ring buffer: at most one queued entry per block at any
   * time (in_worklist guards duplicates), so block_count slots always
   * suffice even though total pushes across refinements are unbounded.
   */
  while (wl_head < wl_tail) {
    size_t block = worklist[wl_head++ % cfg.block_count];
    in_worklist[block] = false;

    size_t start = cfg.block_start[block];
    size_t end = cfg.block_end[block];

    /*
     * Copy current ranges to new_ranges for this block's computation.
     */
    memcpy(new_ranges, ranges, num_values * sizeof(*ranges));

    for (size_t i = start; i < end; ++i) {
      const nyr_inst_t *in = &f->data[i];
      if (in->dst < 0 || (size_t)in->dst >= num_values)
        continue;

      vrp_range_t *dr = &new_ranges[in->dst];

      switch (in->op) {
      case NYIR_CONST_I64:
        vrp_range_const(dr, in->imm);
        break;

      case NYIR_COPY:
        if (in->a >= 0 && (size_t)in->a < num_values) {
          *dr = ranges[in->a];
          if (affines[in->a].valid)
            affines[in->dst] = affines[in->a];
        } else {
          vrp_range_full(dr);
        }
        break;

      /*
       * Conversions produce FLOAT values: copying the integer source range
       * onto the destination would attach an integer interval to a float
       * value id, which downstream consumers read blindly.
       */
      case NYIR_I64_TO_F64:
      case NYIR_I64_TO_F32:
      case NYIR_F64_TO_F32:
      case NYIR_F32_TO_F64:
        vrp_range_full(dr);
        break;

      case NYIR_ADD_I64:
      case NYIR_SUB_I64:
      case NYIR_MUL_I64:
      case NYIR_AND_I64:
      case NYIR_OR_I64:
      case NYIR_XOR_I64:
      case NYIR_SHL_I64:
      case NYIR_SAR_I64:
      case NYIR_DIV_I64:
      case NYIR_MOD_I64:
        if (in->a >= 0 && (size_t)in->a < num_values && in->b >= 0 &&
            (size_t)in->b < num_values) {
          vrp_compute_binop(dr, in->op, &ranges[in->a], &ranges[in->b]);
          if (in->op == NYIR_SUB_I64 && affines[in->a].valid &&
              affines[in->b].valid &&
              affines[in->a].loop_idx == affines[in->b].loop_idx) {
            ny_i128 dinit =
                (ny_i128)affines[in->a].init - affines[in->b].init;
            ny_i128 dstep =
                (ny_i128)affines[in->a].step - affines[in->b].step;
            if (dinit >= INT64_MIN && dinit <= INT64_MAX &&
                dstep >= INT64_MIN && dstep <= INT64_MAX) {
              vrp_range_t exact = {0};
              vrp_seed_affine_bounds(
                  &exact, (int64_t)dinit, (int64_t)dstep,
                  scev.loops[affines[in->a].loop_idx].trip_count);
              if (exact.has_min && exact.has_max) {
                *dr = exact;
                affines[in->dst] = (vrp_affine_t){
                    .valid = true,
                    .loop_idx = affines[in->a].loop_idx,
                    .init = (int64_t)dinit,
                    .step = (int64_t)dstep};
              }
            }
          } else if (in->op == NYIR_ADD_I64 && affines[in->a].valid &&
                     affines[in->b].valid &&
                     affines[in->a].loop_idx == affines[in->b].loop_idx) {
            ny_i128 sinit =
                (ny_i128)affines[in->a].init + affines[in->b].init;
            ny_i128 sstep =
                (ny_i128)affines[in->a].step + affines[in->b].step;
            if (sinit >= INT64_MIN && sinit <= INT64_MAX &&
                sstep >= INT64_MIN && sstep <= INT64_MAX) {
              vrp_range_t exact = {0};
              vrp_seed_affine_bounds(
                  &exact, (int64_t)sinit, (int64_t)sstep,
                  scev.loops[affines[in->a].loop_idx].trip_count);
              if (exact.has_min && exact.has_max) {
                *dr = exact;
                affines[in->dst] = (vrp_affine_t){
                    .valid = true,
                    .loop_idx = affines[in->a].loop_idx,
                    .init = (int64_t)sinit,
                    .step = (int64_t)sstep};
              }
            }
          } else if (in->op == NYIR_SUB_I64 && ranges[in->a].has_min &&
                     ranges[in->a].min >= 0 &&
                     vrp_sub_self_shr(f, defs, num_values, in->a, in->b)) {
            if (!dr->has_min || dr->min < 0) {
              dr->has_min = true;
              dr->min = 0;
            }
            if (ranges[in->a].has_max &&
                (!dr->has_max || dr->max > ranges[in->a].max)) {
              dr->has_max = true;
              dr->max = ranges[in->a].max;
            }
          }
          /*
           * Propagate loop-affine representations across shift/multiply/add/sub
           * with loop constants so derived expressions (e.g. strength-reduced
           * modulo numerators) keep exact non-negative/u32 intervals.
           */
          if (in->op == NYIR_SHL_I64 && in->a >= 0 &&
              (size_t)in->a < num_values && affines[in->a].valid) {
            int64_t sh = 0;
            if (vrp_const_i64(f, defs, num_values, in->b, &sh) && sh >= 0 &&
                sh <= 62) {
              size_t li = affines[in->a].loop_idx;
              ny_i128 scale = ((ny_i128)1) << (int)sh;
              ny_i128 sinit = (ny_i128)affines[in->a].init * scale;
              ny_i128 sstep = (ny_i128)affines[in->a].step * scale;
              if (sinit >= INT64_MIN && sinit <= INT64_MAX &&
                  sstep >= INT64_MIN && sstep <= INT64_MAX) {
                affines[in->dst] = (vrp_affine_t){.valid = true,
                                                  .loop_idx = li,
                                                  .init = (int64_t)sinit,
                                                  .step = (int64_t)sstep};
                if (li < scev.count && scev.loops[li].trip_count_known) {
                  vrp_range_t exact = {0};
                  vrp_seed_affine_bounds(&exact, (int64_t)sinit,
                                         (int64_t)sstep,
                                         scev.loops[li].trip_count);
                  if (exact.has_min && exact.has_max)
                    *dr = exact;
                }
              }
            }
          } else if (in->op == NYIR_MUL_I64) {
            int aff_v = -1;
            int64_t k = 0;
            if (in->a >= 0 && (size_t)in->a < num_values &&
                affines[in->a].valid &&
                vrp_const_i64(f, defs, num_values, in->b, &k))
              aff_v = in->a;
            else if (in->b >= 0 && (size_t)in->b < num_values &&
                     affines[in->b].valid &&
                     vrp_const_i64(f, defs, num_values, in->a, &k))
              aff_v = in->b;
            if (aff_v >= 0) {
              size_t li = affines[aff_v].loop_idx;
              ny_i128 sinit = (ny_i128)affines[aff_v].init * k;
              ny_i128 sstep = (ny_i128)affines[aff_v].step * k;
              if (sinit >= INT64_MIN && sinit <= INT64_MAX &&
                  sstep >= INT64_MIN && sstep <= INT64_MAX) {
                affines[in->dst] = (vrp_affine_t){.valid = true,
                                                  .loop_idx = li,
                                                  .init = (int64_t)sinit,
                                                  .step = (int64_t)sstep};
                if (li < scev.count && scev.loops[li].trip_count_known) {
                  vrp_range_t exact = {0};
                  vrp_seed_affine_bounds(&exact, (int64_t)sinit,
                                         (int64_t)sstep,
                                         scev.loops[li].trip_count);
                  if (exact.has_min && exact.has_max)
                    *dr = exact;
                }
              }
            }
          } else if (in->op == NYIR_ADD_I64 || in->op == NYIR_SUB_I64) {
            int aff_v = -1;
            int64_t c = 0;
            bool const_lhs = false;
            if (in->a >= 0 && (size_t)in->a < num_values &&
                affines[in->a].valid &&
                vrp_const_i64(f, defs, num_values, in->b, &c) &&
                !(in->b >= 0 && (size_t)in->b < num_values &&
                  affines[in->b].valid)) {
              aff_v = in->a;
            } else if (in->b >= 0 && (size_t)in->b < num_values &&
                       affines[in->b].valid &&
                       vrp_const_i64(f, defs, num_values, in->a, &c) &&
                       !(in->a >= 0 && (size_t)in->a < num_values &&
                         affines[in->a].valid)) {
              aff_v = in->b;
              const_lhs = true;
            }
            if (aff_v >= 0) {
              size_t li = affines[aff_v].loop_idx;
              ny_i128 sinit =
                  in->op == NYIR_ADD_I64
                      ? (ny_i128)affines[aff_v].init + c
                      : (const_lhs ? (ny_i128)c - affines[aff_v].init
                                   : (ny_i128)affines[aff_v].init - c);
              ny_i128 sstep =
                  (in->op == NYIR_SUB_I64 && const_lhs)
                      ? -(ny_i128)affines[aff_v].step
                      : (ny_i128)affines[aff_v].step;
              if (sinit >= INT64_MIN && sinit <= INT64_MAX &&
                  sstep >= INT64_MIN && sstep <= INT64_MAX) {
                affines[in->dst] = (vrp_affine_t){.valid = true,
                                                  .loop_idx = li,
                                                  .init = (int64_t)sinit,
                                                  .step = (int64_t)sstep};
                if (li < scev.count && scev.loops[li].trip_count_known) {
                  vrp_range_t exact = {0};
                  vrp_seed_affine_bounds(&exact, (int64_t)sinit,
                                         (int64_t)sstep,
                                         scev.loops[li].trip_count);
                  if (exact.has_min && exact.has_max)
                    *dr = exact;
                }
              }
            }
          }
        } else {
          vrp_range_full(dr);
        }
        break;

      case NYIR_CMP_I64:
        if (in->a >= 0 && (size_t)in->a < num_values && in->b >= 0 &&
            (size_t)in->b < num_values) {
          vrp_compute_cmp(dr, in->cmp, &ranges[in->a], &ranges[in->b]);
        } else {
          vrp_range_full(dr);
        }
        break;

      case NYIR_PHI: {
        if (scev_seeded[in->dst]) {
          *dr = ranges[in->dst];
          break;
        }
        /*
         * Join EVERY incoming range. Truncating at a fixed cap produced a
         * too-narrow join for wide merges (17+ predecessors), and the
         * dropped path's extreme then "proved" bounds it violates.
         */
        vrp_range_t joined;
        size_t joined_count = 0;
        bool saw_unknown = false;
        for (size_t k = 0; k < in->phi_incoming_len; ++k) {
          int val = in->phi_incoming[k].value;
          /*
           * An invalid incoming index is an undef/unknown edge, not an
           * absent one: the path exists and may carry any value.  Joining
           * only the known edges narrows unsoundly (phi(unknown,[0,0])
           * became [0,0] and downstream check-elims trusted it), so
           * widen to top instead of skipping.
           */
          if (val < 0 || (size_t)val >= num_values) {
            saw_unknown = true;
            break;
          }
          if (vrp_resolve_copy(f, defs, num_values, val) == in->dst)
            continue;
          if (joined_count == 0) {
            joined = ranges[val];
          } else {
            vrp_range_t merged;
            vrp_range_t one[2] = {joined, ranges[val]};
            vrp_compute_phi(&merged, 2, one);
            joined = merged;
          }
          ++joined_count;
        }
        if (saw_unknown || joined_count == 0)
          vrp_range_full(dr);
        else
          *dr = joined;
        break;
      }

      case NYIR_LOAD_I64:
      case NYIR_LOAD_LOCAL:
        /*
         * Memory load - conservative.
         */
        vrp_range_full(dr);
        break;

      case NYIR_CALL:
        /*
         * Runtime argc is the tagged encoding of C's nonnegative `int argc`:
         * (argc << 1) | 1.  Keep this exact builtin contract here so range
         * consumers (notably LICM's signed-divide trap check) can use facts
         * that are guaranteed by the process ABI. Other calls stay unknown.
         */
        if (in->symbol &&
            (strcmp(in->symbol, "rt_argc") == 0 ||
             strcmp(in->symbol, "__argc") == 0)) {
          dr->has_min = true;
          dr->has_max = true;
          dr->min = 1;
          dr->max = INT64_C(4294967295);
        } else {
          vrp_range_full(dr);
        }
        break;

      default:
        /*
         * Other ops - conservative.
         */
        vrp_range_full(dr);
        break;
      }

      /*
       * Check if range changed.
       */
      if (dr->has_min != ranges[in->dst].has_min ||
          dr->has_max != ranges[in->dst].has_max ||
          (dr->has_min && dr->min != ranges[in->dst].min) ||
          (dr->has_max && dr->max != ranges[in->dst].max)) {
        ranges[in->dst] = *dr;

        /*
         * Propagate to successors.
         */
        for (size_t e = cfg.succ_offsets[block];
             e < cfg.succ_offsets[block + 1]; ++e) {
          size_t succ = cfg.succ_blocks[e];
          if (!in_worklist[succ]) {
            worklist[wl_tail++ % cfg.block_count] = succ;
            in_worklist[succ] = true;
          }
        }
      }
    }
  }

  /*
   * Instruction ranges are the public fact channel consumed by BCE,
   * overflow elimination, and the verifier's value analysis.  Unknown
   * values must clear stale bounds; otherwise an earlier pass could make an
   * overflow or load look proven.
   */
  for (size_t i = 0; i < f->len; ++i) {
    nyr_inst_t *in = &f->data[i];
    if (in->dst < 0 || (size_t)in->dst >= num_values)
      continue;
    in->range = (nyr_range_t){0};
    /*
     * VRP tracks one-sided bounds internally, but an instruction range is a
     * closed interval: complete an absent side with the trivial bound rather
     * than publishing a fact the verifier rejects.
     */
    if (ranges[in->dst].has_min || ranges[in->dst].has_max) {
      in->range.has_min = true;
      in->range.has_max = true;
      in->range.min = ranges[in->dst].has_min ? ranges[in->dst].min
                                              : INT64_MIN;
      in->range.max = ranges[in->dst].has_max ? ranges[in->dst].max
                                              : INT64_MAX;
    }
  }

  free(ranges);
  free(new_ranges);
  free(affines);
  free(scev_seeded);
  free(in_worklist);
  free(worklist);
  free(defs);
  nyr_cfg_free(&cfg);
  nyr_scev_free(&scev);
  return true;
}
