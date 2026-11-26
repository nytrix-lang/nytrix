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

static bool vrp_array_store_enabled(void) {
  static int disabled = -1;
  if (disabled < 0) {
    const char *e = getenv("NYTRIX_NO_ARRAY_STORE_VRP");
    disabled = (e && e[0] && e[0] != '0') ? 1 : 0;
  }
  return !disabled;
}

/*
 * Compute range for binary operation.
 */
static void vrp_compute_binop(vrp_range_t *dst, nyr_op_t op,
                              const vrp_range_t *a, const vrp_range_t *b) {
  vrp_range_full(dst);
  if (op == NYIR_AND_I64 && vrp_array_store_enabled()) {
    if (b->has_min && b->has_max && b->min >= 0) {
      int64_t hi = (a->has_min && a->has_max && a->min >= 0 && a->max < b->max)
                       ? a->max
                       : b->max;
      dst->has_min = true;
      dst->has_max = true;
      dst->min = 0;
      dst->max = hi;
      return;
    }
    if (a->has_min && a->has_max && a->min >= 0) {
      int64_t hi = (b->has_min && b->has_max && b->min >= 0 && b->max < a->max)
                       ? b->max
                       : a->max;
      dst->has_min = true;
      dst->has_max = true;
      dst->min = 0;
      dst->max = hi;
      return;
    }
  }
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

static bool vrp_ranges_equal(const vrp_range_t *a, const vrp_range_t *b) {
  if (a->has_min != b->has_min || a->has_max != b->has_max)
    return false;
  if (a->has_min && a->min != b->min)
    return false;
  if (a->has_max && a->max != b->max)
    return false;
  return true;
}

static bool vrp_range_is_known(const vrp_range_t *r) {
  return r && r->has_min && r->has_max;
}

static bool vrp_range_subset(const vrp_range_t *sub, const vrp_range_t *sup) {
  if (!vrp_range_is_known(sub) || !vrp_range_is_known(sup))
    return false;
  return sub->min >= sup->min && sub->max <= sup->max;
}

static void vrp_range_union(vrp_range_t *dst, const vrp_range_t *a,
                            const vrp_range_t *b) {
  if (!vrp_range_is_known(a) || !vrp_range_is_known(b)) {
    vrp_range_full(dst);
    return;
  }
  dst->has_min = true;
  dst->has_max = true;
  dst->min = a->min < b->min ? a->min : b->min;
  dst->max = a->max > b->max ? a->max : b->max;
}

/* Strip COPY and additions/subtractions by values proven to equal zero. */
static int vrp_resolve_zero_arith(const nyr_func_t *f, const int *defs,
                                  const vrp_range_t *ranges, size_t num_values,
                                  int v) {
  for (unsigned steps = 0; steps < 8; ++steps) {
    v = vrp_resolve_copy(f, defs, num_values, v);
    if (v < 0 || (size_t)v >= num_values || defs[v] < 0)
      return v;
    const nyr_inst_t *in = &f->data[(size_t)defs[v]];
    if (in->op == NYIR_ADD_I64 && in->a >= 0 && in->b >= 0 &&
        (size_t)in->a < num_values && (size_t)in->b < num_values) {
      if (ranges[in->b].has_min && ranges[in->b].has_max &&
          ranges[in->b].min == 0 && ranges[in->b].max == 0) {
        v = in->a;
        continue;
      }
      if (ranges[in->a].has_min && ranges[in->a].has_max &&
          ranges[in->a].min == 0 && ranges[in->a].max == 0) {
        v = in->b;
        continue;
      }
    } else if (in->op == NYIR_SUB_I64 && in->a >= 0 && in->b >= 0 &&
               (size_t)in->a < num_values && (size_t)in->b < num_values &&
               ranges[in->b].has_min && ranges[in->b].has_max &&
               ranges[in->b].min == 0 && ranges[in->b].max == 0) {
      v = in->a;
      continue;
    }
    return v;
  }
  return v;
}

/*
 * Detect `x - (x & -2^k)` (lowered `x % 2^k` after sign-fixup collapses when
 * `x >= 0`). Returns true and writes `2^k - 1` into `*mod_max_out`.
 */
static bool vrp_sub_self_and_pow2_neg(const nyr_func_t *f, const int *defs,
                                      const vrp_range_t *ranges,
                                      size_t num_values, int lhs, int rhs,
                                      int64_t *mod_max_out) {
  int a = vrp_resolve_copy(f, defs, num_values, lhs);
  int b = vrp_resolve_copy(f, defs, num_values, rhs);
  if (a < 0 || (size_t)a >= num_values || b < 0 || (size_t)b >= num_values ||
      defs[b] < 0)
    return false;
  const nyr_inst_t *bin = &f->data[(size_t)defs[b]];
  if (bin->op != NYIR_AND_I64)
    return false;
  int64_t mask = 0;
  int inner = -1;
  if (vrp_const_i64(f, defs, num_values, bin->b, &mask))
    inner = bin->a;
  else if (vrp_const_i64(f, defs, num_values, bin->a, &mask))
    inner = bin->b;
  else
    return false;
  if (mask >= 0 || mask == INT64_MIN)
    return false;
  uint64_t pow2 = (uint64_t)(-mask);
  if (pow2 == 0 || (pow2 & (pow2 - 1u)) != 0 || pow2 - 1u > (uint64_t)INT64_MAX)
    return false;
  int root = vrp_resolve_zero_arith(f, defs, ranges, num_values, inner);
  if (root != a)
    return false;
  *mod_max_out = (int64_t)(pow2 - 1u);
  return true;
}

#define VRP_MEM_WIDTH_MASK                                                     \
  (NYIR_INST_F_MEM_BYTE | NYIR_INST_F_MEM_U16 | NYIR_INST_F_MEM_I32)

typedef struct {
  int root_val;
  size_t alloc_inst;
  size_t init_call_inst;
  bool is_tbuf;
  bool zero_init;
  bool valid;
  bool has_access;
  unsigned width_mask;
  bool has_range;
  vrp_range_t range;
} vrp_buf_root_t;

typedef struct {
  vrp_buf_root_t *roots;
  size_t count;
  int *val_root;
  bool *val_has_add_offset;
  int *val_add_offset;
} vrp_buf_info_t;

static unsigned vrp_width_bytes(unsigned width_mask) {
  if (width_mask & NYIR_INST_F_MEM_BYTE)
    return 1u;
  if (width_mask & NYIR_INST_F_MEM_U16)
    return 2u;
  if (width_mask & NYIR_INST_F_MEM_I32)
    return 4u;
  return 8u;
}

static void vrp_invalidate_root(vrp_buf_info_t *bi, int r) {
  if (r >= 0 && (size_t)r < bi->count)
    bi->roots[r].valid = false;
}

static bool vrp_mem_op_supported(const nyr_inst_t *in, unsigned *width_out) {
  if (in->flags & (NYIR_INST_F_MEM_VOLATILE | NYIR_INST_F_MEM_SEG_FS |
                   NYIR_INST_F_MEM_SEG_GS))
    return false;
  if (in->scalar_class == NYIR_SCALAR_F32 ||
      in->scalar_class == NYIR_SCALAR_F64)
    return false;
  *width_out = in->flags & VRP_MEM_WIDTH_MASK;
  /* Disallow combined multi-width flags if malformed. */
  unsigned w = *width_out;
  if (w != 0 && (w & (w - 1u)) != 0)
    return false;
  return in->imm >= 0;
}

/*
 * Recognize `rt_tbuf_new_typed_raw` calls and zero-initialized `NYIR_ALLOCA`s
 * whose pointers never escape `f` and only serve uniform-width integer accesses.
 */
static void vrp_analyze_buf_roots(const nyr_func_t *f, const int *defs,
                                  const nyr_cfg_t *cfg, size_t num_values,
                                  vrp_buf_info_t *bi) {
  memset(bi, 0, sizeof(*bi));
  size_t cap = 0;
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->dst < 0 || (size_t)in->dst >= num_values)
      continue;
    bool is_tbuf =
        (in->op == NYIR_CALL && in->symbol &&
         strcmp(in->symbol, "rt_tbuf_new_typed_raw") == 0);
    bool is_alloca = (in->op == NYIR_ALLOCA && in->imm > 0);
    if (!is_tbuf && !is_alloca)
      continue;
    size_t init_call = SIZE_MAX;
    if (is_alloca) {
      size_t blk = cfg->inst_block[i];
      size_t bend = blk < cfg->block_count ? cfg->block_end[blk] : f->len;
      for (size_t j = i + 1; j < bend; ++j) {
        const nyr_inst_t *nx = &f->data[j];
        if (nx->op == NYIR_NOP || nx->op == NYIR_CONST_I64 ||
            nx->op == NYIR_COPY)
          continue;
        if (nx->op == NYIR_CALL && nx->symbol &&
            strcmp(nx->symbol, "rt_memset") == 0 && nx->imm == 3 &&
            vrp_resolve_copy(f, defs, num_values, nx->a) == in->dst) {
          int64_t fill = -1, bytes = -1;
          if (vrp_const_i64(f, defs, num_values, nx->b, &fill) && fill == 0 &&
              vrp_const_i64(f, defs, num_values, nx->c, &bytes) &&
              bytes == in->imm) {
            init_call = j;
          }
        }
        break;
      }
      if (init_call == SIZE_MAX)
        continue;
    }
    if (bi->count == cap) {
      size_t ncap = cap ? cap * 2 : 8;
      vrp_buf_root_t *nr = realloc(bi->roots, ncap * sizeof(*nr));
      if (!nr)
        return;
      bi->roots = nr;
      cap = ncap;
    }
    bi->roots[bi->count++] = (vrp_buf_root_t){.root_val = in->dst,
                                              .alloc_inst = i,
                                              .init_call_inst = init_call,
                                              .is_tbuf = is_tbuf,
                                              .zero_init = true,
                                              .valid = true};
  }
  if (bi->count == 0)
    return;

  bi->val_root = malloc(num_values * sizeof(*bi->val_root));
  bi->val_has_add_offset =
      calloc(num_values, sizeof(*bi->val_has_add_offset));
  bi->val_add_offset = malloc(num_values * sizeof(*bi->val_add_offset));
  if (!bi->val_root || !bi->val_has_add_offset || !bi->val_add_offset) {
    free(bi->roots);
    free(bi->val_root);
    free(bi->val_has_add_offset);
    free(bi->val_add_offset);
    memset(bi, 0, sizeof(*bi));
    return;
  }
  for (size_t v = 0; v < num_values; ++v) {
    bi->val_root[v] = -1;
    bi->val_add_offset[v] = -1;
  }
  for (size_t r = 0; r < bi->count; ++r)
    bi->val_root[bi->roots[r].root_val] = (int)r;

  /* Propagate unique pointer root provenance across COPY and ADD_I64. */
  for (unsigned pass = 0; pass < 16; ++pass) {
    bool changed = false;
    for (size_t i = 0; i < f->len; ++i) {
      const nyr_inst_t *in = &f->data[i];
      if (in->dst < 0 || (size_t)in->dst >= num_values)
        continue;
      int next_r = -1;
      bool has_off = false;
      int off_v = -1;
      if (in->op == NYIR_COPY && in->a >= 0 && (size_t)in->a < num_values) {
        next_r = bi->val_root[in->a];
      } else if (in->op == NYIR_ADD_I64 && in->a >= 0 &&
                 (size_t)in->a < num_values && in->b >= 0 &&
                 (size_t)in->b < num_values) {
        int ra = bi->val_root[in->a];
        int rb = bi->val_root[in->b];
        if (ra != -1 && rb != -1) {
          vrp_invalidate_root(bi, ra);
          vrp_invalidate_root(bi, rb);
          next_r = -2;
        } else if (ra != -1) {
          next_r = ra;
          has_off = true;
          off_v = in->b;
        } else if (rb != -1) {
          next_r = rb;
          has_off = true;
          off_v = in->a;
        }
      }
      if (next_r == -1)
        continue;
      int cur = bi->val_root[in->dst];
      if (cur == -1) {
        bi->val_root[in->dst] = next_r;
        bi->val_has_add_offset[in->dst] = has_off;
        bi->val_add_offset[in->dst] = off_v;
        changed = true;
      } else if (cur != next_r) {
        vrp_invalidate_root(bi, cur);
        vrp_invalidate_root(bi, next_r);
        if (cur != -2) {
          bi->val_root[in->dst] = -2;
          changed = true;
        }
      }
    }
    if (!changed)
      break;
  }

  /* Verify non-escaping and uniform-width integer access for every use. */
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    int slots[6] = {in->a, in->b, in->c, in->d, in->e, in->f};
    if (in->op == NYIR_PHI) {
      if (in->dst >= 0 && (size_t)in->dst < num_values)
        vrp_invalidate_root(bi, bi->val_root[in->dst]);
      for (size_t k = 0; k < in->phi_incoming_len; ++k) {
        int val = in->phi_incoming[k].value;
        if (val >= 0 && (size_t)val < num_values)
          vrp_invalidate_root(bi, bi->val_root[val]);
      }
      continue;
    }
    for (size_t k = 0; k < in->extra_args_len; ++k) {
      int val = in->extra_args[k];
      if (val >= 0 && (size_t)val < num_values)
        vrp_invalidate_root(bi, bi->val_root[val]);
    }
    for (size_t k = 0; k < 6; ++k) {
      int v = slots[k];
      if (v < 0 || (size_t)v >= num_values)
        continue;
      int r = bi->val_root[v];
      if (r < 0)
        continue;
      if (r == -2 || (size_t)r >= bi->count)
        continue;
      if (in->op == NYIR_COPY && k == 0)
        continue;
      if (in->op == NYIR_ADD_I64 && (k == 0 || k == 1)) {
        int other = slots[1 - k];
        if (other >= 0 && (size_t)other < num_values &&
            bi->val_root[other] != -1)
          vrp_invalidate_root(bi, r);
        continue;
      }
      if (in->op == NYIR_BOUNDS_CHECK && k == 0)
        continue;
      if (in->op == NYIR_CALL && k == 0 && i == bi->roots[r].init_call_inst)
        continue;
      if ((in->op == NYIR_LOAD_I64 || in->op == NYIR_STORE_I64) && k == 0) {
        unsigned w = 0;
        if (!vrp_mem_op_supported(in, &w)) {
          vrp_invalidate_root(bi, r);
          continue;
        }
        if (!bi->roots[r].has_access) {
          bi->roots[r].has_access = true;
          bi->roots[r].width_mask = w;
        } else if (bi->roots[r].width_mask != w) {
          vrp_invalidate_root(bi, r);
          continue;
        }
        if (w != NYIR_INST_F_MEM_BYTE && in->imm != 0)
          vrp_invalidate_root(bi, r);
        continue;
      }
      vrp_invalidate_root(bi, r);
    }
  }
}

static void vrp_buf_info_free(vrp_buf_info_t *bi) {
  free(bi->roots);
  free(bi->val_root);
  free(bi->val_has_add_offset);
  free(bi->val_add_offset);
  memset(bi, 0, sizeof(*bi));
}

/* Prove nonnegativity and optional element alignment for an address offset. */
static bool vrp_ptr_offset_safe(const nyr_func_t *f, const int *defs,
                                const vrp_range_t *ranges, size_t num_values,
                                int off_v, unsigned align_bytes,
                                bool require_nonneg) {
  if (off_v < 0 || (size_t)off_v >= num_values)
    return false;
  if (require_nonneg && (!ranges[off_v].has_min || ranges[off_v].min < 0))
    return false;
  if (align_bytes <= 1u)
    return true;
  int64_t c = 0;
  if (vrp_const_i64(f, defs, num_values, off_v, &c))
    return (c % (int64_t)align_bytes) == 0;
  int cur = vrp_resolve_copy(f, defs, num_values, off_v);
  if (cur < 0 || (size_t)cur >= num_values || defs[cur] < 0)
    return false;
  const nyr_inst_t *in = &f->data[(size_t)defs[cur]];
  if (in->op == NYIR_SHL_I64 &&
      vrp_const_i64(f, defs, num_values, in->b, &c) && c >= 0 && c <= 16)
    return ((UINT64_C(1) << (unsigned)c) % align_bytes) == 0;
  if (in->op == NYIR_MUL_I64) {
    if (vrp_const_i64(f, defs, num_values, in->b, &c) &&
        (c % (int64_t)align_bytes) == 0)
      return true;
    if (vrp_const_i64(f, defs, num_values, in->a, &c) &&
        (c % (int64_t)align_bytes) == 0)
      return true;
  }
  return false;
}

static bool vrp_addr_chain_safe(const nyr_func_t *f, const int *defs,
                                const vrp_range_t *ranges, size_t num_values,
                                const vrp_buf_info_t *bi, int addr_v, int r,
                                unsigned align_bytes, bool require_nonneg) {
  int cur = addr_v;
  for (unsigned steps = 0; steps < 16; ++steps) {
    if (cur < 0 || (size_t)cur >= num_values || bi->val_root[cur] != r)
      return false;
    if (cur == bi->roots[r].root_val)
      return true;
    if (defs[cur] < 0)
      return false;
    const nyr_inst_t *in = &f->data[(size_t)defs[cur]];
    if (in->op == NYIR_COPY) {
      cur = in->a;
      continue;
    }
    if (in->op == NYIR_ADD_I64 && bi->val_has_add_offset[cur]) {
      if (!vrp_ptr_offset_safe(f, defs, ranges, num_values,
                               bi->val_add_offset[cur], align_bytes,
                               require_nonneg))
        return false;
      cur = (bi->val_root[in->a] == r) ? in->a : in->b;
      continue;
    }
    return false;
  }
  return false;
}

/* Recompute joined store+init value ranges for each valid buffer root. */
static bool vrp_compute_buf_ranges(const nyr_func_t *f, const int *defs,
                                   const vrp_range_t *ranges, size_t num_values,
                                   vrp_buf_info_t *bi) {
  if (!bi || bi->count == 0)
    return false;
  bool any_changed = false;
  for (size_t r = 0; r < bi->count; ++r) {
    vrp_buf_root_t *root = &bi->roots[r];
    if (!root->valid || !root->has_access) {
      if (root->has_range) {
        root->has_range = false;
        vrp_range_full(&root->range);
        any_changed = true;
      }
      continue;
    }
    unsigned align_bytes = vrp_width_bytes(root->width_mask);
    bool addrs_ok = true;
    bool has_range = root->zero_init;
    vrp_range_t joined = {0};
    if (root->zero_init)
      vrp_range_const(&joined, 0);

    for (size_t i = 0; i < f->len; ++i) {
      const nyr_inst_t *in = &f->data[i];
      if ((in->op != NYIR_LOAD_I64 && in->op != NYIR_STORE_I64) ||
          in->a < 0 || (size_t)in->a >= num_values ||
          bi->val_root[in->a] != (int)r)
        continue;
      /* Typed heap buffers carry metadata header words before index 0; verify
       * nonnegative indices so header reads/writes can never be conflated. */
      if (!vrp_addr_chain_safe(f, defs, ranges, num_values, bi, in->a, (int)r,
                               align_bytes, root->is_tbuf)) {
        addrs_ok = false;
        break;
      }
      if (in->op != NYIR_STORE_I64)
        continue;
      if (in->c < 0 || (size_t)in->c >= num_values) {
        has_range = false;
        break;
      }
      vrp_range_t sr = ranges[in->c];
      vrp_range_t clamped = {0};
      if (root->width_mask == NYIR_INST_F_MEM_BYTE) {
        clamped.has_min = clamped.has_max = true;
        if (vrp_range_is_known(&sr) && sr.min >= 0 && sr.max <= UINT8_MAX) {
          clamped.min = sr.min;
          clamped.max = sr.max;
        } else {
          clamped.min = 0;
          clamped.max = UINT8_MAX;
        }
      } else if (root->width_mask == NYIR_INST_F_MEM_U16) {
        clamped.has_min = clamped.has_max = true;
        if (vrp_range_is_known(&sr) && sr.min >= 0 && sr.max <= UINT16_MAX) {
          clamped.min = sr.min;
          clamped.max = sr.max;
        } else {
          clamped.min = 0;
          clamped.max = UINT16_MAX;
        }
      } else if (root->width_mask == NYIR_INST_F_MEM_I32) {
        clamped.has_min = clamped.has_max = true;
        if (vrp_range_is_known(&sr) && sr.min >= 0 &&
            sr.max <= (int64_t)UINT32_MAX) {
          clamped.min = sr.min;
          clamped.max = sr.max;
        } else {
          clamped.min = 0;
          clamped.max = (int64_t)UINT32_MAX;
        }
      } else {
        if (!vrp_range_is_known(&sr)) {
          has_range = false;
          break;
        }
        clamped = sr;
      }
      if (!has_range) {
        joined = clamped;
        has_range = true;
      } else {
        vrp_range_union(&joined, &joined, &clamped);
      }
    }

    bool next_has = addrs_ok && has_range;
    vrp_range_t next_r = next_has ? joined : (vrp_range_t){0};
    if (next_has != root->has_range ||
        (next_has && !vrp_ranges_equal(&next_r, &root->range))) {
      root->has_range = next_has;
      root->range = next_r;
      any_changed = true;
    }
  }
  return any_changed;
}

/* Evaluate one NYIR instruction's output range from `ranges` (and optional
 * SCEV affines and tracked private buffer store ranges). */
static void vrp_eval_inst_range(const nyr_func_t *f, const int *defs,
                                size_t num_values, const nyr_scev_info_t *scev,
                                const vrp_range_t *ranges, vrp_affine_t *affines,
                                const bool *scev_seeded,
                                const vrp_buf_info_t *buf_info,
                                const nyr_inst_t *in, vrp_range_t *dr) {
  switch (in->op) {
  case NYIR_CONST_I64:
    vrp_range_const(dr, in->imm);
    break;

  case NYIR_COPY:
    if (in->a >= 0 && (size_t)in->a < num_values) {
      *dr = ranges[in->a];
      if (affines && affines[in->a].valid)
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
      if (affines && in->op == NYIR_SUB_I64 && affines[in->a].valid &&
          affines[in->b].valid &&
          affines[in->a].loop_idx == affines[in->b].loop_idx) {
        ny_i128 dinit = (ny_i128)affines[in->a].init - affines[in->b].init;
        ny_i128 dstep = (ny_i128)affines[in->a].step - affines[in->b].step;
        if (dinit >= INT64_MIN && dinit <= INT64_MAX && dstep >= INT64_MIN &&
            dstep <= INT64_MAX) {
          vrp_range_t exact = {0};
          vrp_seed_affine_bounds(
              &exact, (int64_t)dinit, (int64_t)dstep,
              scev->loops[affines[in->a].loop_idx].trip_count);
          if (exact.has_min && exact.has_max) {
            *dr = exact;
            affines[in->dst] = (vrp_affine_t){
                .valid = true,
                .loop_idx = affines[in->a].loop_idx,
                .init = (int64_t)dinit,
                .step = (int64_t)dstep};
          }
        }
      } else if (affines && in->op == NYIR_ADD_I64 && affines[in->a].valid &&
                 affines[in->b].valid &&
                 affines[in->a].loop_idx == affines[in->b].loop_idx) {
        ny_i128 sinit = (ny_i128)affines[in->a].init + affines[in->b].init;
        ny_i128 sstep = (ny_i128)affines[in->a].step + affines[in->b].step;
        if (sinit >= INT64_MIN && sinit <= INT64_MAX && sstep >= INT64_MIN &&
            sstep <= INT64_MAX) {
          vrp_range_t exact = {0};
          vrp_seed_affine_bounds(
              &exact, (int64_t)sinit, (int64_t)sstep,
              scev->loops[affines[in->a].loop_idx].trip_count);
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
      } else if (vrp_array_store_enabled() && in->op == NYIR_SUB_I64 &&
                 ranges[in->a].has_min && ranges[in->a].min >= 0) {
        int64_t mod_max = 0;
        if (vrp_sub_self_and_pow2_neg(f, defs, ranges, num_values, in->a, in->b,
                                      &mod_max)) {
          if (ranges[in->a].has_max && ranges[in->a].max < mod_max)
            mod_max = ranges[in->a].max;
          if (!dr->has_min || dr->min < 0) {
            dr->has_min = true;
            dr->min = 0;
          }
          if (!dr->has_max || dr->max > mod_max) {
            dr->has_max = true;
            dr->max = mod_max;
          }
        }
      }
      /*
       * Propagate loop-affine representations across shift/multiply/add/sub
       * with loop constants so derived expressions (e.g. strength-reduced
       * modulo numerators) keep exact non-negative/u32 intervals.
       */
      if (affines && in->op == NYIR_SHL_I64 && in->a >= 0 &&
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
            if (li < scev->count && scev->loops[li].trip_count_known) {
              vrp_range_t exact = {0};
              vrp_seed_affine_bounds(&exact, (int64_t)sinit, (int64_t)sstep,
                                     scev->loops[li].trip_count);
              if (exact.has_min && exact.has_max)
                *dr = exact;
            }
          }
        }
      } else if (affines && in->op == NYIR_MUL_I64) {
        int aff_v = -1;
        int64_t k = 0;
        if (in->a >= 0 && (size_t)in->a < num_values && affines[in->a].valid &&
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
            if (li < scev->count && scev->loops[li].trip_count_known) {
              vrp_range_t exact = {0};
              vrp_seed_affine_bounds(&exact, (int64_t)sinit, (int64_t)sstep,
                                     scev->loops[li].trip_count);
              if (exact.has_min && exact.has_max)
                *dr = exact;
            }
          }
        }
      } else if (affines && (in->op == NYIR_ADD_I64 || in->op == NYIR_SUB_I64)) {
        int aff_v = -1;
        int64_t c = 0;
        bool const_lhs = false;
        if (in->a >= 0 && (size_t)in->a < num_values && affines[in->a].valid &&
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
          ny_i128 sinit = in->op == NYIR_ADD_I64
                              ? (ny_i128)affines[aff_v].init + c
                              : (const_lhs ? (ny_i128)c - affines[aff_v].init
                                           : (ny_i128)affines[aff_v].init - c);
          ny_i128 sstep = (in->op == NYIR_SUB_I64 && const_lhs)
                              ? -(ny_i128)affines[aff_v].step
                              : (ny_i128)affines[aff_v].step;
          if (sinit >= INT64_MIN && sinit <= INT64_MAX &&
              sstep >= INT64_MIN && sstep <= INT64_MAX) {
            affines[in->dst] = (vrp_affine_t){.valid = true,
                                              .loop_idx = li,
                                              .init = (int64_t)sinit,
                                              .step = (int64_t)sstep};
            if (li < scev->count && scev->loops[li].trip_count_known) {
              vrp_range_t exact = {0};
              vrp_seed_affine_bounds(&exact, (int64_t)sinit, (int64_t)sstep,
                                     scev->loops[li].trip_count);
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
    if (scev_seeded && scev_seeded[in->dst]) {
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
    if (buf_info && buf_info->count > 0 && in->a >= 0 &&
        (size_t)in->a < num_values) {
      int r = buf_info->val_root[in->a];
      if (r >= 0 && (size_t)r < buf_info->count &&
          buf_info->roots[r].valid && buf_info->roots[r].has_range) {
        *dr = buf_info->roots[r].range;
        break;
      }
    }
    if (vrp_array_store_enabled() && (in->flags & NYIR_INST_F_MEM_BYTE) &&
        !(in->flags & (NYIR_INST_F_MEM_VOLATILE | NYIR_INST_F_MEM_SEG_FS |
                       NYIR_INST_F_MEM_SEG_GS)) &&
        in->scalar_class != NYIR_SCALAR_F32 &&
        in->scalar_class != NYIR_SCALAR_F64) {
      dr->has_min = true;
      dr->has_max = true;
      dr->min = 0;
      dr->max = UINT8_MAX;
      break;
    }
    vrp_range_full(dr);
    break;

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
}

/*
 * Test whether assuming `cand[phi_dst] = guess` makes `cand[back_v]` close
 * soundly inside `[guess.min, guess.max]` across `in_loop` blocks.
 */
static vrp_range_t vrp_eval_inductive_body(
    const nyr_func_t *f, const int *defs, const nyr_cfg_t *cfg,
    const nyr_scev_info_t *scev, const vrp_range_t *base_ranges,
    vrp_range_t *cand, const bool *in_loop, size_t hb, int phi_dst,
    vrp_range_t guess, const vrp_buf_info_t *buf_info, int back_v,
    size_t num_values) {
  memcpy(cand, base_ranges, num_values * sizeof(*cand));
  for (size_t b = 0; b < cfg->block_count; ++b) {
    if (!in_loop[b])
      continue;
    for (size_t i = cfg->block_start[b]; i < cfg->block_end[b]; ++i) {
      const nyr_inst_t *in = &f->data[i];
      if (in->dst < 0 || (size_t)in->dst >= num_values)
        continue;
      if (b == hb && in->op == NYIR_PHI)
        continue;
      vrp_range_full(&cand[in->dst]);
    }
  }
  cand[phi_dst] = guess;
  for (int sweep = 0; sweep < 2; ++sweep) {
    for (size_t b = 0; b < cfg->block_count; ++b) {
      if (!in_loop[b])
        continue;
      for (size_t i = cfg->block_start[b]; i < cfg->block_end[b]; ++i) {
        const nyr_inst_t *in = &f->data[i];
        if (in->dst < 0 || (size_t)in->dst >= num_values)
          continue;
        if (b == hb && in->op == NYIR_PHI)
          continue;
        vrp_range_t tmp = {0};
        vrp_eval_inst_range(f, defs, num_values, scev, cand, NULL, NULL,
                            buf_info, in, &tmp);
        cand[in->dst] = tmp;
      }
    }
  }
  return cand[back_v];
}

/*
 * Prove bounded inductive intervals for loop-carried recurrences that clamp
 * via modulo/AND (e.g., LCG `seed = (seed * A + C) % M`) and therefore cannot
 * be solved as linear SCEV affine forms.
 */
static bool vrp_prove_inductive_phis(
    const nyr_func_t *f, const int *defs, const nyr_cfg_t *cfg,
    const nyr_scev_info_t *scev, vrp_range_t *ranges, vrp_range_t *cand,
    const bool *scev_seeded, const vrp_buf_info_t *buf_info, bool *in_worklist,
    size_t *worklist, size_t *wl_tail, size_t num_values) {
  bool *in_loop = malloc(cfg->block_count * sizeof(*in_loop));
  size_t *q = malloc(cfg->block_count * sizeof(*q));
  if (!in_loop || !q) {
    free(in_loop);
    free(q);
    return false;
  }
  bool improved = false;
  for (size_t hb = 0; hb < cfg->block_count; ++hb) {
    for (size_t i = cfg->block_start[hb]; i < cfg->block_end[hb]; ++i) {
      const nyr_inst_t *phi = &f->data[i];
      if (phi->op == NYIR_LABEL || phi->op == NYIR_NOP)
        continue;
      if (phi->op != NYIR_PHI)
        break;
      if (phi->dst < 0 || (size_t)phi->dst >= num_values ||
          scev_seeded[phi->dst] || vrp_range_is_known(&ranges[phi->dst]) ||
          phi->phi_incoming_len != 2 || !phi->phi_incoming)
        continue;
      int v0 = phi->phi_incoming[0].value, v1 = phi->phi_incoming[1].value;
      int64_t l0 = phi->phi_incoming[0].predecessor_label;
      int64_t l1 = phi->phi_incoming[1].predecessor_label;
      if (v0 < 0 || v1 < 0 || (size_t)v0 >= num_values ||
          (size_t)v1 >= num_values)
        continue;
      int pre_v = -1, back_v = -1;
      int64_t pre_lbl = -1, back_lbl = -1;
      if (vrp_range_is_known(&ranges[v0]) && !vrp_range_is_known(&ranges[v1])) {
        pre_v = v0;
        pre_lbl = l0;
        back_v = v1;
        back_lbl = l1;
      } else if (vrp_range_is_known(&ranges[v1]) &&
                 !vrp_range_is_known(&ranges[v0])) {
        pre_v = v1;
        pre_lbl = l1;
        back_v = v0;
        back_lbl = l0;
      } else {
        continue;
      }
      size_t pre_b = 0, back_b = 0;
      if (!nyr_cfg_block_for_label(cfg, pre_lbl, &pre_b) ||
          !nyr_cfg_block_for_label(cfg, back_lbl, &back_b) || back_b == hb)
        continue;
      memset(in_loop, 0, cfg->block_count * sizeof(*in_loop));
      in_loop[hb] = true;
      in_loop[back_b] = true;
      size_t qh = 0, qt = 0;
      q[qt++] = back_b;
      while (qh < qt) {
        size_t b = q[qh++];
        for (size_t e = cfg->pred_offsets[b]; e < cfg->pred_offsets[b + 1];
             ++e) {
          size_t pb = cfg->pred_blocks[e];
          if (!in_loop[pb]) {
            in_loop[pb] = true;
            q[qt++] = pb;
          }
        }
      }
      if (in_loop[pre_b] || (hb != 0 && in_loop[0]))
        continue;

      vrp_range_t r0 = ranges[pre_v];
      vrp_range_t r1 = vrp_eval_inductive_body(
          f, defs, cfg, scev, ranges, cand, in_loop, hb, phi->dst, r0,
          buf_info, back_v, num_values);
      if (!vrp_range_is_known(&r1))
        continue;
      vrp_range_t guess;
      vrp_range_union(&guess, &r0, &r1);
      bool proven = false;
      for (int attempt = 0; attempt < 2; ++attempt) {
        vrp_range_t next_r = vrp_eval_inductive_body(
            f, defs, cfg, scev, ranges, cand, in_loop, hb, phi->dst, guess,
            buf_info, back_v, num_values);
        if (!vrp_range_is_known(&next_r))
          break;
        if (vrp_range_subset(&next_r, &guess)) {
          proven = true;
          break;
        }
        vrp_range_t next_guess;
        vrp_range_union(&next_guess, &guess, &next_r);
        guess = next_guess;
      }
      if (proven) {
        ranges[phi->dst] = guess;
        for (size_t e = cfg->succ_offsets[hb]; e < cfg->succ_offsets[hb + 1];
             ++e) {
          size_t succ = cfg->succ_blocks[e];
          if (!in_worklist[succ]) {
            worklist[(*wl_tail)++ % cfg->block_count] = succ;
            in_worklist[succ] = true;
          }
        }
        if (!in_worklist[hb]) {
          worklist[(*wl_tail)++ % cfg->block_count] = hb;
          in_worklist[hb] = true;
        }
        improved = true;
      }
    }
  }
  free(in_loop);
  free(q);
  return improved;
}

static void vrp_run_worklist(const nyr_func_t *f, const int *defs,
                             const nyr_cfg_t *cfg, const nyr_scev_info_t *scev,
                             vrp_range_t *ranges, vrp_range_t *new_ranges,
                             vrp_affine_t *affines, const bool *scev_seeded,
                             const vrp_buf_info_t *buf_info, bool *in_worklist,
                             size_t *worklist, size_t *wl_head, size_t *wl_tail,
                             size_t num_values) {
  while (*wl_head < *wl_tail) {
    size_t block = worklist[(*wl_head)++ % cfg->block_count];
    in_worklist[block] = false;

    size_t start = cfg->block_start[block];
    size_t end = cfg->block_end[block];
    memcpy(new_ranges, ranges, num_values * sizeof(*ranges));

    for (size_t i = start; i < end; ++i) {
      const nyr_inst_t *in = &f->data[i];
      if (in->dst < 0 || (size_t)in->dst >= num_values)
        continue;

      vrp_range_t *dr = &new_ranges[in->dst];
      vrp_eval_inst_range(f, defs, num_values, scev, ranges, affines,
                          scev_seeded, buf_info, in, dr);

      if (!vrp_ranges_equal(dr, &ranges[in->dst])) {
        ranges[in->dst] = *dr;
        for (size_t e = cfg->succ_offsets[block];
             e < cfg->succ_offsets[block + 1]; ++e) {
          size_t succ = cfg->succ_blocks[e];
          if (!in_worklist[succ]) {
            worklist[(*wl_tail)++ % cfg->block_count] = succ;
            in_worklist[succ] = true;
          }
        }
      }
    }
  }
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

  vrp_buf_info_t buf_info = {0};
  bool use_store_vrp = vrp_array_store_enabled();
  if (use_store_vrp)
    vrp_analyze_buf_roots(f, defs, &cfg, num_values, &buf_info);

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
  vrp_run_worklist(f, defs, &cfg, &scev, ranges, new_ranges, affines,
                   scev_seeded, NULL, in_worklist, worklist, &wl_head,
                   &wl_tail, num_values);

  if (use_store_vrp) {
    for (int iter = 0; iter < 8; ++iter) {
      bool re_queued = vrp_prove_inductive_phis(
          f, defs, &cfg, &scev, ranges, new_ranges, scev_seeded, &buf_info,
          in_worklist, worklist, &wl_tail, num_values);
      if (vrp_compute_buf_ranges(f, defs, ranges, num_values, &buf_info)) {
        for (size_t i = 0; i < f->len; ++i) {
          const nyr_inst_t *in = &f->data[i];
          if (in->op != NYIR_LOAD_I64 || in->dst < 0 || in->a < 0 ||
              (size_t)in->a >= num_values)
            continue;
          int r = buf_info.val_root[in->a];
          if (r >= 0 && (size_t)r < buf_info.count &&
              buf_info.roots[r].valid) {
            size_t lb = cfg.inst_block[i];
            if (lb < cfg.block_count && !in_worklist[lb]) {
              worklist[wl_tail++ % cfg.block_count] = lb;
              in_worklist[lb] = true;
            }
            re_queued = true;
          }
        }
      }
      if (!re_queued && wl_head >= wl_tail)
        break;
      vrp_run_worklist(f, defs, &cfg, &scev, ranges, new_ranges, affines,
                       scev_seeded, &buf_info, in_worklist, worklist, &wl_head,
                       &wl_tail, num_values);
    }
  }
  vrp_buf_info_free(&buf_info);

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
