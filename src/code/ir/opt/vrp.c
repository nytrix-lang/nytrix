/*
 * VRP: value range propagation through SSA.
 *
 * ── Literature ──
 *  Patterson — "Accurate Static Branch Prediction by Value Range
 *   Propagation" (PLDI 1995). Interval lattice with forward/
 *   backward propagation. §3 transfer functions; §4 narrowing.
 *
 *  GCC tree-vrp.c: range lattice [min,max] feeding branch
 *   prediction, BCE, and overflow elimination.
 *
 * ── Nytrix implementation ──
 *  Forward CFG propagation with narrowing at conditionals.
 *  Consumed by IRCE, BCE, overflow/null check elimination.
 */
#include "code/ir/opt/util.h"
#include <stdlib.h>
#include <limits.h>


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
static void vrp_compute_binop(vrp_range_t *dst, nyir_op_t op,
                              const vrp_range_t *a, const vrp_range_t *b) {
  vrp_range_full(dst);
  if (!a->has_min || !a->has_max || !b->has_min || !b->has_max)
    return;

  int64_t amin = a->min, amax = a->max;
  int64_t bmin = b->min, bmax = b->max;
  __int128 rmin, rmax;

  switch (op) {
  case NYIR_ADD_I64:
    rmin = (__int128)amin + bmin;
    rmax = (__int128)amax + bmax;
    break;
  case NYIR_SUB_I64:
    rmin = (__int128)amin - bmax;
    rmax = (__int128)amax - bmin;
    break;
  case NYIR_MUL_I64: {
    __int128 vals[4] = {
      (__int128)amin * bmin,
      (__int128)amin * bmax,
      (__int128)amax * bmin,
      (__int128)amax * bmax
    };
    rmin = vals[0];
    rmax = vals[0];
    for (int i = 1; i < 4; ++i) {
      if (vals[i] < rmin) rmin = vals[i];
      if (vals[i] > rmax) rmax = vals[i];
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
    rmax = (__int128)mask;
    break;
  }
  case NYIR_SHL_I64:
    if (amin < 0 || bmin < 0 || bmax >= 63)
      return;
    rmin = (__int128)amin << (int)bmin;
    rmax = (__int128)amax << (int)bmax;
    break;
  case NYIR_SAR_I64: {
    if (bmin < 0 || bmax >= 64)
      return;
    int64_t vals[4] = {
      amin >> (int)bmin, amin >> (int)bmax,
      amax >> (int)bmin, amax >> (int)bmax
    };
    rmin = vals[0];
    rmax = vals[0];
    for (int i = 1; i < 4; ++i) {
      if (vals[i] < rmin) rmin = vals[i];
      if (vals[i] > rmax) rmax = vals[i];
    }
    break;
  }
  case NYIR_DIV_I64: {
    if (bmin <= 0 && bmax >= 0)
      return;
    int64_t av[2] = {amin, amax};
    int64_t bv[2] = {bmin, bmax};
    __int128 vals[4];
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
      if (vals[i] < rmin) rmin = vals[i];
      if (vals[i] > rmax) rmax = vals[i];
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
    rmin = amax <= 0 ? -(__int128)magnitude :
           amin >= 0 ? 0 : -(__int128)magnitude;
    rmax = amin >= 0 ? magnitude :
           amax <= 0 ? 0 : magnitude;
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
static void vrp_compute_cmp(vrp_range_t *dst, nyir_cmp_t cmp,
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
             (a->min == a->max && b->min == b->max &&
              a->min != b->min);
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
bool nyir_vrp(nyir_func_t *f) {
  if (!f || f->next_value <= 0)
    return true;

  int *defs = nyir_build_defs(f);
  nyir_cfg_t cfg = {0};
  if (!defs || !nyir_cfg_build(f, &cfg)) {
    free(defs);
    nyir_cfg_free(&cfg);
    return false;
  }

  size_t num_values = (size_t)f->next_value;
  vrp_range_t *ranges = calloc(num_values, sizeof(*ranges));
  vrp_range_t *new_ranges = calloc(num_values, sizeof(*new_ranges));
  bool *in_worklist = calloc(cfg.block_count, sizeof(*in_worklist));
  size_t *worklist = calloc(cfg.block_count, sizeof(*worklist));
  size_t wl_head = 0, wl_tail = 0;

  if (!ranges || !new_ranges || !in_worklist || !worklist) {
    free(ranges);
    free(new_ranges);
    free(in_worklist);
    free(worklist);
    free(defs);
    nyir_cfg_free(&cfg);
    return false;
  }

  /*
   * Initialize: constants have known ranges.
   */
  for (size_t i = 0; i < f->len; ++i) {
    const nyir_inst_t *in = &f->data[i];
    if (in->op == NYIR_CONST_I64 && in->dst >= 0 &&
        (size_t)in->dst < num_values) {
      vrp_range_const(&ranges[in->dst], in->imm);
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
      const nyir_inst_t *in = &f->data[i];
      if (in->dst < 0 || (size_t)in->dst >= num_values)
        continue;

      vrp_range_t *dr = &new_ranges[in->dst];

      switch (in->op) {
      case NYIR_CONST_I64:
        vrp_range_const(dr, in->imm);
        break;

      case NYIR_COPY:
        if (in->a >= 0 && (size_t)in->a < num_values)
          *dr = ranges[in->a];
        else
          vrp_range_full(dr);
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
        if (in->a >= 0 && (size_t)in->a < num_values &&
            in->b >= 0 && (size_t)in->b < num_values) {
          vrp_compute_binop(dr, in->op, &ranges[in->a], &ranges[in->b]);
        } else {
          vrp_range_full(dr);
        }
        break;

      case NYIR_CMP_I64:
        if (in->a >= 0 && (size_t)in->a < num_values &&
            in->b >= 0 && (size_t)in->b < num_values) {
          vrp_compute_cmp(dr, in->cmp, &ranges[in->a], &ranges[in->b]);
        } else {
          vrp_range_full(dr);
        }
        break;

      case NYIR_PHI: {
        /*
         * Join EVERY incoming range. Truncating at a fixed cap produced a
         * too-narrow join for wide merges (17+ predecessors), and the
         * dropped path's extreme then "proved" bounds it violates.
         */
        vrp_range_t joined;
        size_t joined_count = 0;
        for (size_t k = 0; k < in->phi_incoming_len; ++k) {
          int val = in->phi_incoming[k].value;
          if (val < 0 || (size_t)val >= num_values)
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
        if (joined_count == 0)
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
         * Function call - conservative unless pure.
         */
        vrp_range_full(dr);
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
        for (size_t e = cfg.succ_offsets[block]; e < cfg.succ_offsets[block + 1]; ++e) {
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
    nyir_inst_t *in = &f->data[i];
    if (in->dst < 0 || (size_t)in->dst >= num_values)
      continue;
    in->range = (nyir_range_t){0};
    if (ranges[in->dst].has_min) {
      in->range.has_min = true;
      in->range.min = ranges[in->dst].min;
    }
    if (ranges[in->dst].has_max) {
      in->range.has_max = true;
      in->range.max = ranges[in->dst].max;
    }
  }

  free(ranges);
  free(new_ranges);
  free(in_worklist);
  free(worklist);
  free(defs);
  nyir_cfg_free(&cfg);
  return true;
}