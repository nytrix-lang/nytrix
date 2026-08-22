/*
 * SLP vectorization: groups isomorphic independent scalar operations
 * into packed SIMD instructions using a bottom-up tree-matching pass.
 */
#include "code/native/ir/opt/util.h"
#include "code/native/ir/internal.h"
#include "base/compat.h"
#include "base/common.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

/*
 * Straight-line two-lane SLP for the V128 i64/f64 subset.
 *
 * Current backends represent NYIR_VEC4_* in 128 bits, so the legal pack width
 * here is two 64-bit lanes.  We form complete expression trees rooted at a
 * pair of contiguous stores.  A scalar definition is erased only when all of
 * its uses belong to the corresponding lane of that tree; this avoids needing
 * extract-lane operations that NYIR does not currently expose.  For f64, an
 * identical constant operand may instead be broadcast once and shared by both
 * lanes.
 */

#define SLP_I64_WIDTH 2

typedef struct {
  bool ok;
  int root;
  int64_t offset;
} addr_form_t;

static bool slp_binop(nyir_op_t op, bool f64) {
  if (f64)
    return op == NYIR_ADD_F64 || op == NYIR_SUB_F64 ||
           op == NYIR_MUL_F64 || op == NYIR_DIV_F64;
  return op == NYIR_ADD_I64 || op == NYIR_SUB_I64 ||
         op == NYIR_AND_I64 || op == NYIR_OR_I64 ||
         op == NYIR_XOR_I64;
}

static bool slp_commutative(nyir_op_t op) {
  return op == NYIR_ADD_I64 || op == NYIR_AND_I64 ||
         op == NYIR_OR_I64 || op == NYIR_XOR_I64 ||
         op == NYIR_ADD_F64 || op == NYIR_MUL_F64;
}

static nyir_op_t slp_vec_binop(nyir_op_t op) {
  switch (op) {
  case NYIR_ADD_I64: return NYIR_VEC4_ADD_I64;
  case NYIR_SUB_I64: return NYIR_VEC4_SUB_I64;
  case NYIR_AND_I64: return NYIR_VEC4_AND_I64;
  case NYIR_OR_I64:  return NYIR_VEC4_OR_I64;
  case NYIR_XOR_I64: return NYIR_VEC4_XOR_I64;
  case NYIR_ADD_F64: return NYIR_VEC4_ADD_F64;
  case NYIR_SUB_F64: return NYIR_VEC4_SUB_F64;
  case NYIR_MUL_F64: return NYIR_VEC4_MUL_F64;
  case NYIR_DIV_F64: return NYIR_VEC4_DIV_F64;
  default: return NYIR_NOP;
  }
}

static nyir_op_t slp_vec8_i64_binop(nyir_op_t op) {
  switch (op) {
  case NYIR_ADD_I64: return NYIR_VEC8_ADD_I64;
  case NYIR_SUB_I64: return NYIR_VEC8_SUB_I64;
  case NYIR_AND_I64: return NYIR_VEC8_AND_I64;
  case NYIR_OR_I64:  return NYIR_VEC8_OR_I64;
  case NYIR_XOR_I64: return NYIR_VEC8_XOR_I64;
  default: return NYIR_NOP;
  }
}

static bool slp_f64_broadcastable(const nyir_func_t *f, const int *defs, int v) {
  if (!f || !defs || v < 0 || v >= f->next_value || defs[v] < 0)
    return false;
  return f->data[(size_t)defs[v]].op == NYIR_CONST_F64;
}

static bool slp_const(const nyir_func_t *f, const int *defs, int v, int64_t *out) {
  if (!f || !defs || v < 0 || v >= f->next_value || defs[v] < 0)
    return false;
  const nyir_inst_t *in = &f->data[(size_t)defs[v]];
  if (in->op != NYIR_CONST_I64)
    return false;
  if (out) *out = in->imm;
  return true;
}

static addr_form_t slp_addr(const nyir_func_t *f, const int *defs, int value,
                            size_t block_start, unsigned depth) {
  addr_form_t bad = {0};
  if (!f || !defs || value < 0 || value >= f->next_value || depth > 16)
    return bad;
  int di = defs[value];
  if (di < 0 || (size_t)di < block_start)
    return (addr_form_t){.ok = true, .root = value, .offset = 0};
  const nyir_inst_t *in = &f->data[(size_t)di];
  if (in->op == NYIR_ADDR_LOCAL || in->op == NYIR_ADDR_SYMBOL || in->op == NYIR_ALLOCA)
    return (addr_form_t){.ok = true, .root = value, .offset = 0};
  if (in->op == NYIR_COPY)
    return slp_addr(f, defs, in->a, block_start, depth + 1);
  if (in->op == NYIR_ADD_I64 || in->op == NYIR_SUB_I64) {
    int64_t k = 0;
    int base = -1;
    bool subtract = in->op == NYIR_SUB_I64;
    if (slp_const(f, defs, in->b, &k)) {
      base = in->a;
      if (subtract) {
        if (k == INT64_MIN)
          return bad;
        k = -k;
      }
    } else if (!subtract && slp_const(f, defs, in->a, &k)) {
      base = in->b;
    } else {
      return bad;
    }
    addr_form_t a = slp_addr(f, defs, base, block_start, depth + 1);
    if (!a.ok || (__int128)a.offset + k < INT64_MIN ||
        (__int128)a.offset + k > INT64_MAX)
      return bad;
    a.offset += k;
    return a;
  }
  return (addr_form_t){.ok = true, .root = value, .offset = 0};
}

static bool slp_contiguous(const nyir_func_t *f, const int *defs,
                           int a0, int a1, size_t block_start) {
  addr_form_t x = slp_addr(f, defs, a0, block_start, 0);
  addr_form_t y = slp_addr(f, defs, a1, block_start, 0);
  return x.ok && y.ok && x.root == y.root &&
         (__int128)y.offset - (__int128)x.offset == 8;
}

static bool slp_contiguous4(const nyir_func_t *f, const int *defs,
                            const int addr[4], size_t block_start) {
  addr_form_t a[4];
  for (size_t lane = 0; lane < 4; ++lane) {
    a[lane] = slp_addr(f, defs, addr[lane], block_start, 0);
    if (!a[lane].ok || a[lane].root != a[0].root)
      return false;
  }
  for (size_t lane = 1; lane < 4; ++lane)
    if ((__int128)a[lane].offset - (__int128)a[0].offset !=
        (__int128)lane * 8)
      return false;
  return true;
}

static bool slp_value_only_used_at(const nyir_use_def_t *uses, int value,
                                   size_t expected) {
  if (!uses || value < 0 || (size_t)value >= uses->value_count)
    return false;
  size_t begin = uses->offsets[value], end = uses->offsets[value + 1];
  if (begin == end)
    return false;
  for (size_t i = begin; i < end; ++i)
    if (uses->users[i] != expected)
      return false;
  return true;
}

static bool slp_no_observable_between(const nyir_func_t *f, size_t a, size_t b) {
  if (!f) return false;
  if (a > b) { size_t t = a; a = b; b = t; }
  for (size_t i = a + 1; i < b; ++i) {
    const nyir_inst_t *in = &f->data[i];
    unsigned effects = in->effects | nyir_inst_effects(in);
    if (effects & (NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_CALL |
                   NYIR_EFFECT_MAY_TRAP | NYIR_EFFECT_VOLATILE |
                   NYIR_EFFECT_UNKNOWN_SIDE_EFFECT))
      return false;
  }
  return true;
}

static bool slp_no_effect_between(const nyir_func_t *f, size_t a, size_t b) {
  if (!f) return false;
  if (a > b) { size_t t = a; a = b; b = t; }
  for (size_t i = a + 1; i < b; ++i) {
    const nyir_inst_t *in = &f->data[i];
    if (in->op == NYIR_NOP)
      continue;
    if ((in->effects | nyir_inst_effects(in)) != NYIR_EFFECT_NONE ||
        in->op == NYIR_BR || in->op == NYIR_BR_IF || in->op == NYIR_RET)
      return false;
  }
  return true;
}

/*
 * Pack values v0/v1.  expected0/1 are their sole lane users in the original
 * use-def graph.  The packed result deliberately reuses v0's SSA number.
 */
static bool slp_pack_pair(nyir_func_t *f, const int *defs,
                          const nyir_use_def_t *uses, int v0, int v1,
                          size_t expected0, size_t expected1,
                          size_t block_start, bool f64, unsigned depth,
                          int *out_vec) {
  if (!f || !defs || !uses || !out_vec || depth > 12 || v0 < 0 || v1 < 0 ||
      v0 >= f->next_value || v1 >= f->next_value || defs[v0] < 0 || defs[v1] < 0)
    return false;
  if (!slp_value_only_used_at(uses, v0, expected0) ||
      !slp_value_only_used_at(uses, v1, expected1))
    return false;

  size_t d0 = (size_t)defs[v0], d1 = (size_t)defs[v1];
  if (d0 < block_start || d1 < block_start)
    return false;
  nyir_inst_t i0 = f->data[d0];
  nyir_inst_t i1 = f->data[d1];
  if (i0.op != i1.op)
    return false;

  if (i0.op == NYIR_LOAD_I64) {
    bool l0_f64 = (i0.flags & NYIR_INST_F_MEM_F64) != 0;
    bool l1_f64 = (i1.flags & NYIR_INST_F_MEM_F64) != 0;
    if (l0_f64 != f64 || l1_f64 != f64 ||
        !slp_contiguous(f, defs, i0.a, i1.a, block_start) ||
        !slp_no_observable_between(f, d0, d1))
      return false;
    size_t keep = d0 > d1 ? d0 : d1;
    size_t kill = d0 > d1 ? d1 : d0;
    /*
     * Lane zero is always the first address, independent of instruction order.
     */
    nyir_inst_discard(&f->data[kill]);
    f->data[keep] = (nyir_inst_t){.op = f64 ? NYIR_VEC4_LOAD_F64
                                             : NYIR_VEC4_LOAD_I64,
                                  .dst = v0, .a = i0.a, .b = -1, .c = -1,
                                  .d = -1, .e = -1, .f = -1,
                                  .effects = NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_MAY_TRAP};
    *out_vec = v0;
    return true;
  }

  if (!slp_binop(i0.op, f64))
    return false;
  if (i0.a < 0 || i0.b < 0 || i1.a < 0 || i1.b < 0)
    return false;

  /*
   * Normalize a shared f64 constant to the RHS for commutative operations.
   * This lets expressions such as scale*x0, scale*x1 use the same broadcast
   * path as x0*scale, x1*scale without mutating either scalar constant.
   */
  if (f64 && slp_commutative(i0.op)) {
    /*
     * Canonicalize a broadcastable scalar constant to the RHS per lane.
     */
    if (slp_f64_broadcastable(f, defs, i0.a) &&
        !slp_f64_broadcastable(f, defs, i0.b)) {
      int t = i0.a; i0.a = i0.b; i0.b = t;
    }
    if (slp_f64_broadcastable(f, defs, i1.a) &&
        !slp_f64_broadcastable(f, defs, i1.b)) {
      int t = i1.a; i1.a = i1.b; i1.b = t;
    }
  }

  bool shared_f64_rhs = f64 && i0.b == i1.b &&
                        slp_f64_broadcastable(f, defs, i0.b);
  int va = -1, vb = -1;
  bool first_ok = slp_pack_pair(f, defs, uses, i0.a, i1.a,
                                d0, d1, block_start, f64, depth + 1, &va);
  bool second_ok = false;
  if (first_ok) {
    if (i0.b == i0.a && i1.b == i1.a) {
      vb = va;
      second_ok = true;
    } else if (shared_f64_rhs) {
      /*
       * Materialize the broadcast into the lane definition that is erased.
       */
      vb = f->next_value++;
      second_ok = true;
    } else {
      second_ok = slp_pack_pair(f, defs, uses, i0.b, i1.b,
                                d0, d1, block_start, f64, depth + 1, &vb);
    }
  }
  if ((!first_ok || !second_ok) && slp_commutative(i0.op)) {
    /*
     * Keep the transform conservative instead of guessing cross-lane operand
     * pairing after a recursive pack has already mutated this candidate.
     */
    return false;
  }
  if (!first_ok || !second_ok)
    return false;

  size_t keep = d0 > d1 ? d0 : d1;
  size_t kill = d0 > d1 ? d1 : d0;
  if (shared_f64_rhs) {
    f->data[kill] = (nyir_inst_t){.op = NYIR_VEC4_SET1_F64, .dst = vb,
                                  .a = i0.b, .b = -1, .c = -1, .d = -1,
                                  .e = -1, .f = -1,
                                  .effects = NYIR_EFFECT_NONE};
  } else {
    nyir_inst_discard(&f->data[kill]);
  }
  f->data[keep] = (nyir_inst_t){.op = slp_vec_binop(i0.op), .dst = v0,
                                .a = va, .b = vb, .c = -1, .d = -1,
                                .e = -1, .f = -1,
                                .effects = NYIR_EFFECT_NONE};
  *out_vec = v0;
  return true;
}


static bool slp_quad_load_group_safe(const nyir_func_t *f,
                                     const size_t at[4]) {
  if (!f || !at)
    return false;
  size_t lo = at[0], hi = at[0];
  for (size_t lane = 1; lane < 4; ++lane) {
    if (at[lane] < lo) lo = at[lane];
    if (at[lane] > hi) hi = at[lane];
  }
  for (size_t i = lo; i <= hi; ++i) {
    bool lane_load = false;
    for (size_t lane = 0; lane < 4; ++lane)
      if (i == at[lane]) { lane_load = true; break; }
    if (lane_load || f->data[i].op == NYIR_NOP)
      continue;
    /*
     * Do not move/widen a potentially trapping load across unrelated work.
     */
    return false;
  }
  return true;
}

static bool slp_pack_quad_load_i64(nyir_func_t *f, const int *defs,
                                   const nyir_use_def_t *uses,
                                   const int value[4], const size_t user[4],
                                   size_t block_start, int *out_vec) {
  size_t at[4];
  int addr[4];
  for (size_t lane = 0; lane < 4; ++lane) {
    if (value[lane] < 0 || value[lane] >= f->next_value || defs[value[lane]] < 0 ||
        !slp_value_only_used_at(uses, value[lane], user[lane]))
      return false;
    at[lane] = (size_t)defs[value[lane]];
    if (at[lane] < block_start || f->data[at[lane]].op != NYIR_LOAD_I64 ||
        (f->data[at[lane]].flags & NYIR_INST_F_MEM_F64))
      return false;
    addr[lane] = f->data[at[lane]].a;
  }
  if (!slp_contiguous4(f, defs, addr, block_start))
    return false;
  size_t lo = at[0], hi = at[0];
  for (size_t lane = 1; lane < 4; ++lane) {
    if (at[lane] < lo) lo = at[lane];
    if (at[lane] > hi) hi = at[lane];
  }
  if (!slp_quad_load_group_safe(f, at))
    return false;
  for (size_t lane = 0; lane < 4; ++lane)
    if (at[lane] != hi)
      nyir_inst_discard(&f->data[at[lane]]);
  f->data[hi] = (nyir_inst_t){.op = NYIR_VEC8_LOAD_I64, .dst = value[0],
                              .a = addr[0], .b = -1, .c = -1, .d = -1,
                              .e = -1, .f = -1,
                              .effects = NYIR_EFFECT_READ_MEMORY |
                                         NYIR_EFFECT_MAY_TRAP};
  *out_vec = value[0];
  return true;
}

/*
 * Four-lane i64 SLP fast path for the common straight-line shape
 *   load a[0..3], load b[0..3], lane-wise ALU, store c[0..3].
 * It deliberately requires every scalar load/result to be lane-private and
 * all loads to precede all ALU nodes, avoiding extract-lane requirements and
 * preserving textual SSA ordering.
 */
static bool slp_try_store_quad_i64(nyir_func_t *f, const size_t store[4],
                                   size_t block_start) {
  if (!f)
    return false;
  int *defs = nyir_build_defs(f);
  nyir_use_def_t uses = {0};
  if (!defs || !nyir_build_use_def(f, &uses)) {
    free(defs);
    return false;
  }
  bool ok = true;
  int store_addr[4], result[4], lhs[4], rhs[4];
  size_t bin_at[4];
  nyir_op_t scalar_op = NYIR_NOP;
  size_t min_bin = SIZE_MAX, max_bin = 0;
  for (size_t lane = 0; lane < 4 && ok; ++lane) {
    if (store[lane] >= f->len) { ok = false; break; }
    const nyir_inst_t *st = &f->data[store[lane]];
    if (st->op != NYIR_STORE_I64 ||
        (st->flags & NYIR_INST_F_MEM_F64) || st->c < 0) {
      ok = false; break;
    }
    store_addr[lane] = st->a;
    result[lane] = st->c;
    if (result[lane] >= f->next_value || defs[result[lane]] < 0 ||
        !slp_value_only_used_at(&uses, result[lane], store[lane])) {
      ok = false; break;
    }
    bin_at[lane] = (size_t)defs[result[lane]];
    const nyir_inst_t *bin = &f->data[bin_at[lane]];
    if (!slp_binop(bin->op, false) ||
        slp_vec8_i64_binop(bin->op) == NYIR_NOP || bin->a < 0 || bin->b < 0) {
      ok = false; break;
    }
    if (lane == 0) scalar_op = bin->op;
    else if (bin->op != scalar_op) { ok = false; break; }
    lhs[lane] = bin->a;
    rhs[lane] = bin->b;
    if (bin_at[lane] < min_bin) min_bin = bin_at[lane];
    if (bin_at[lane] > max_bin) max_bin = bin_at[lane];
  }
  if (ok && !slp_contiguous4(f, defs, store_addr, block_start))
    ok = false;
  if (ok) {
    for (size_t lane = 0; lane < 4; ++lane)
      if (bin_at[lane] >= store[lane]) { ok = false; break; }
  }

  int vlhs = -1, vrhs = -1;
  bool same_operands = true;
  for (size_t lane = 0; lane < 4; ++lane)
    if (lhs[lane] != rhs[lane]) { same_operands = false; break; }
  size_t lhs_max_def = 0, rhs_max_def = 0;
  if (ok) {
    for (size_t lane = 0; lane < 4; ++lane) {
      if (lhs[lane] < 0 || lhs[lane] >= f->next_value || defs[lhs[lane]] < 0) {
        ok = false; break;
      }
      size_t di = (size_t)defs[lhs[lane]];
      if (di > lhs_max_def) lhs_max_def = di;
    }
    if (lhs_max_def >= min_bin)
      ok = false;
  }
  if (ok && !same_operands) {
    for (size_t lane = 0; lane < 4; ++lane) {
      if (rhs[lane] < 0 || rhs[lane] >= f->next_value || defs[rhs[lane]] < 0) {
        ok = false; break;
      }
      size_t di = (size_t)defs[rhs[lane]];
      if (di > rhs_max_def) rhs_max_def = di;
    }
    if (rhs_max_def >= min_bin)
      ok = false;
  }

  if (ok)
    ok = slp_pack_quad_load_i64(f, defs, &uses, lhs, bin_at,
                                block_start, &vlhs);
  if (ok) {
    if (same_operands) {
      vrhs = vlhs;
    } else {
      ok = slp_pack_quad_load_i64(f, defs, &uses, rhs, bin_at,
                                  block_start, &vrhs);
    }
  }
  if (ok) {
    for (size_t lane = 0; lane < 4; ++lane)
      if (bin_at[lane] != max_bin)
        nyir_inst_discard(&f->data[bin_at[lane]]);
    f->data[max_bin] = (nyir_inst_t){.op = slp_vec8_i64_binop(scalar_op),
                                     .dst = result[0], .a = vlhs, .b = vrhs,
                                     .c = -1, .d = -1, .e = -1, .f = -1,
                                     .effects = NYIR_EFFECT_NONE};
    size_t keep = store[3];
    for (size_t lane = 0; lane < 3; ++lane)
      nyir_inst_discard(&f->data[store[lane]]);
    f->data[keep] = (nyir_inst_t){.op = NYIR_VEC8_STORE_I64, .dst = -1,
                                  .a = store_addr[0], .b = result[0],
                                  .c = -1, .d = -1, .e = -1, .f = -1,
                                  .effects = NYIR_EFFECT_WRITE_MEMORY |
                                             NYIR_EFFECT_MAY_TRAP};
  }
  nyir_use_def_free(&uses);
  free(defs);
  return ok;
}

static bool slp_try_store_pair(nyir_func_t *f, size_t s0, size_t s1,
                               size_t block_start) {
  if (!f || s0 >= f->len || s1 >= f->len || s0 == s1)
    return false;
  const nyir_inst_t st0 = f->data[s0], st1 = f->data[s1];
  if (st0.op != NYIR_STORE_I64 || st1.op != NYIR_STORE_I64 ||
      st0.c < 0 || st1.c < 0 || !slp_no_effect_between(f, s0, s1))
    return false;

  int *defs = nyir_build_defs(f);
  nyir_use_def_t uses = {0};
  if (!defs || !nyir_build_use_def(f, &uses)) {
    free(defs);
    return false;
  }
  bool st0_f64 = (st0.flags & NYIR_INST_F_MEM_F64) != 0;
  bool st1_f64 = (st1.flags & NYIR_INST_F_MEM_F64) != 0;
  bool f64 = st0_f64 && st1_f64;
  bool ok = st0_f64 == st1_f64 &&
            slp_contiguous(f, defs, st0.a, st1.a, block_start);
  int packed = -1;
  if (ok)
    ok = slp_pack_pair(f, defs, &uses, st0.c, st1.c,
                       s0, s1, block_start, f64, 0, &packed);
  if (ok) {
    size_t keep = s0 > s1 ? s0 : s1;
    size_t kill = s0 > s1 ? s1 : s0;
    nyir_inst_discard(&f->data[kill]);
    f->data[keep] = (nyir_inst_t){.op = f64 ? NYIR_VEC4_STORE_F64 : NYIR_VEC4_STORE_I64,
                                  .dst = -1, .a = st0.a, .b = packed, .c = -1,
                                  .d = -1, .e = -1, .f = -1,
                                  .effects = NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP};
  }
  nyir_use_def_free(&uses);
  free(defs);
  return ok;
}

static size_t slp_next_non_nop_in_block(const nyir_func_t *f, size_t at, size_t end) {
  while (at < end && f->data[at].op == NYIR_NOP)
    at++;
  return at;
}

bool nyir_slp_vectorize(nyir_func_t *f) {
  if (!f || f->len < 4 || f->next_value <= 0)
    return true;

  nyir_func_t work = {0};
  if (!nyir_func_clone(f, &work))
    return false;
  bool changed = false;

  for (unsigned round = 0; round < 64; ++round) {
    nyir_cfg_t cfg = {0};
    if (!nyir_cfg_build(&work, &cfg)) {
      nyir_func_free(&work);
      return false;
    }
    bool transformed = false;
    for (size_t b = 0; b < cfg.block_count && !transformed; ++b) {
      size_t start = cfg.block_start[b], end = cfg.block_end[b];
      for (size_t i = start; i < end && !transformed; ++i) {
        if (work.data[i].op != NYIR_STORE_I64)
          continue;
        size_t stores[4] = {i, SIZE_MAX, SIZE_MAX, SIZE_MAX};
        for (size_t lane = 1; lane < 4; ++lane) {
          stores[lane] = slp_next_non_nop_in_block(&work, stores[lane - 1] + 1, end);
          if (stores[lane] >= end || work.data[stores[lane]].op != NYIR_STORE_I64)
            break;
        }
        if (stores[3] < end && work.data[stores[3]].op == NYIR_STORE_I64) {
          nyir_func_t quad = {0};
          if (!nyir_func_clone(&work, &quad)) {
            nyir_cfg_free(&cfg);
            nyir_func_free(&work);
            return false;
          }
          if (slp_try_store_quad_i64(&quad, stores, start)) {
            char err[256] = {0};
            if (nyir_verify(&quad, err, sizeof(err))) {
              nyir_func_free(&work);
              work = quad;
              transformed = true;
              changed = true;
              break;
            }
          }
          nyir_func_free(&quad);
        }
        size_t j = slp_next_non_nop_in_block(&work, i + 1, end);
        if (j >= end || work.data[j].op != NYIR_STORE_I64)
          continue;

        nyir_func_t candidate = {0};
        if (!nyir_func_clone(&work, &candidate)) {
          nyir_cfg_free(&cfg);
          nyir_func_free(&work);
          return false;
        }
        if (slp_try_store_pair(&candidate, i, j, start)) {
          char err[256] = {0};
          if (nyir_verify(&candidate, err, sizeof(err))) {
            nyir_func_free(&work);
            work = candidate;
            transformed = true;
            changed = true;
          } else {
            nyir_func_free(&candidate);
          }
        } else {
          nyir_func_free(&candidate);
        }
      }
    }
    nyir_cfg_free(&cfg);
    if (!transformed)
      break;
  }

  if (!changed) {
    nyir_func_free(&work);
    return true;
  }
  nyir_func_free(f);
  *f = work;
  return true;
}
