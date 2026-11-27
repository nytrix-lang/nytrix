/*
 * SLP vectorization: groups isomorphic independent scalar operations
 * into packed SIMD instructions using a bottom-up tree-matching pass.
 */
#include "base/common.h"
#include "base/compat.h"
#include "base/debug.h"
#include "code/backend/core/nyr.h"
#include "code/backend/passes/util.h"
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

/*
 * Candidate undo journal.
 *
 * Each candidate rewrite used to run on a full clone of the function, which
 * made a long body with many rejected store pairs quadratic. Rewrites only
 * overwrite or discard existing instruction slots and allocate new value
 * numbers, so the work body records each slot's first prior value and the
 * value counter instead. A rejected candidate restores them; an accepted one
 * releases the discarded slots' storage. Discards defer their frees so a
 * rollback can restore the original instruction.
 */
typedef struct {
  size_t index;
  nyr_inst_t old;
} slp_journal_entry_t;

typedef struct {
  slp_journal_entry_t *items;
  size_t len;
  size_t cap;
  int next_value;
  bool failed;
} slp_journal_t;

static _Thread_local slp_journal_t *slp_journal;

static void slp_journal_save(const nyr_func_t *f, size_t k) {
  slp_journal_t *j = slp_journal;
  if (!j)
    return;
  for (size_t i = 0; i < j->len; ++i)
    if (j->items[i].index == k)
      return;
  if (j->len == j->cap) {
    size_t cap = j->cap ? j->cap * 2 : 16;
    slp_journal_entry_t *items = realloc(j->items, cap * sizeof(*items));
    if (!items) {
      j->failed = true;
      return;
    }
    j->items = items;
    j->cap = cap;
  }
  j->items[j->len++] = (slp_journal_entry_t){.index = k, .old = f->data[k]};
}

static void slp_discard(nyr_func_t *f, size_t k) {
  if (!slp_journal) {
    nyr_inst_discard(&f->data[k]);
    return;
  }
  slp_journal_save(f, k);
  f->data[k] = (nyr_inst_t){.op = NYIR_NOP,
                            .dst = -1,
                            .a = -1,
                            .b = -1,
                            .c = -1,
                            .d = -1,
                            .e = -1,
                            .f = -1};
}

static void slp_assign(nyr_func_t *f, size_t k, nyr_inst_t in) {
  slp_journal_save(f, k);
  f->data[k] = in;
}

static void slp_journal_begin(slp_journal_t *j, const nyr_func_t *f) {
  j->len = 0;
  j->failed = false;
  j->next_value = f->next_value;
  slp_journal = j;
}

static void slp_journal_rollback(slp_journal_t *j, nyr_func_t *f) {
  for (size_t i = j->len; i-- > 0;)
    f->data[j->items[i].index] = j->items[i].old;
  f->next_value = j->next_value;
  j->len = 0;
  slp_journal = NULL;
}

static void slp_journal_commit(slp_journal_t *j, const nyr_func_t *f);

/*
 * Commit an accepted candidate. The work body is re-cloned so it matches the
 * clone-per-candidate form exactly (the clone normalizes the symbol pool and
 * empty debug file names). On failure `work` is freed.
 */
static bool slp_accept(slp_journal_t *j, nyr_func_t *work) {
  slp_journal_commit(j, work);
  nyr_func_t fresh = {0};
  bool ok = nyr_ir_func_clone(work, &fresh);
  nyr_ir_func_free(work);
  if (!ok)
    return false;
  *work = fresh;
  return true;
}

static void slp_journal_commit(slp_journal_t *j, const nyr_func_t *f) {
  for (size_t i = 0; i < j->len; ++i) {
    const nyr_inst_t *now = &f->data[j->items[i].index];
    nyr_inst_t *old = &j->items[i].old;
    if (old->extra_args != now->extra_args)
      free(old->extra_args);
    if (old->arg_sizes != now->arg_sizes)
      free(old->arg_sizes);
    if (old->phi_incoming != now->phi_incoming)
      free(old->phi_incoming);
  }
  j->len = 0;
  slp_journal = NULL;
}

typedef struct {
  bool ok;
  int root;
  int64_t offset;
} addr_form_t;

static bool slp_binop(nyr_op_t op, bool f64) {
  if (f64)
    return op == NYIR_ADD_F64 || op == NYIR_SUB_F64 || op == NYIR_MUL_F64 ||
           op == NYIR_DIV_F64;
  return op == NYIR_ADD_I64 || op == NYIR_SUB_I64 || op == NYIR_AND_I64 ||
         op == NYIR_OR_I64 || op == NYIR_XOR_I64;
}

static bool slp_commutative(nyr_op_t op) {
  return op == NYIR_ADD_I64 || op == NYIR_AND_I64 || op == NYIR_OR_I64 ||
         op == NYIR_XOR_I64 || op == NYIR_ADD_F64 || op == NYIR_MUL_F64;
}

static nyr_op_t slp_vec_binop(nyr_op_t op) {
  switch (op) {
  case NYIR_ADD_I64:
    return NYIR_VEC4_ADD_I64;
  case NYIR_SUB_I64:
    return NYIR_VEC4_SUB_I64;
  case NYIR_AND_I64:
    return NYIR_VEC4_AND_I64;
  case NYIR_OR_I64:
    return NYIR_VEC4_OR_I64;
  case NYIR_XOR_I64:
    return NYIR_VEC4_XOR_I64;
  case NYIR_ADD_F64:
    return NYIR_VEC4_ADD_F64;
  case NYIR_SUB_F64:
    return NYIR_VEC4_SUB_F64;
  case NYIR_MUL_F64:
    return NYIR_VEC4_MUL_F64;
  case NYIR_DIV_F64:
    return NYIR_VEC4_DIV_F64;
  default:
    return NYIR_NOP;
  }
}

static nyr_op_t slp_vec8_i64_binop(nyr_op_t op) {
  switch (op) {
  case NYIR_ADD_I64:
    return NYIR_VEC8_ADD_I64;
  case NYIR_SUB_I64:
    return NYIR_VEC8_SUB_I64;
  case NYIR_AND_I64:
    return NYIR_VEC8_AND_I64;
  case NYIR_OR_I64:
    return NYIR_VEC8_OR_I64;
  case NYIR_XOR_I64:
    return NYIR_VEC8_XOR_I64;
  default:
    return NYIR_NOP;
  }
}

static bool slp_f64_broadcastable(const nyr_func_t *f, const int *defs, int v) {
  if (!f || !defs || v < 0 || v >= f->next_value || defs[v] < 0)
    return false;
  return f->data[(size_t)defs[v]].op == NYIR_CONST_F64;
}

static addr_form_t slp_addr(const nyr_func_t *f, const int *defs, int value,
                            size_t block_start, unsigned depth) {
  (void)block_start;
  addr_form_t bad = {0};
  if (!f || !defs || value < 0 || value >= f->next_value || depth > 16)
    return bad;
  int di = defs[value];
  if (di < 0 || (size_t)di >= f->len)
    return (addr_form_t){.ok = true, .root = value, .offset = 0};
  const nyr_inst_t *in = &f->data[(size_t)di];
  if (in->op == NYIR_ADDR_LOCAL || in->op == NYIR_ADDR_SYMBOL ||
      in->op == NYIR_ALLOCA)
    return (addr_form_t){.ok = true, .root = value, .offset = 0};
  if (in->op == NYIR_COPY)
    return slp_addr(f, defs, in->a, block_start, depth + 1);
  if (in->op == NYIR_ADD_I64 || in->op == NYIR_SUB_I64) {
    int64_t k = 0;
    int base = -1;
    bool subtract = in->op == NYIR_SUB_I64;
    if (nyr_const_i64(f, defs, in->b, &k)) {
      base = in->a;
      if (subtract) {
        if (k == INT64_MIN)
          return bad;
        k = -k;
      }
    } else if (!subtract && nyr_const_i64(f, defs, in->a, &k)) {
      base = in->b;
    } else {
      return (addr_form_t){.ok = true, .root = value, .offset = 0};
    }
    addr_form_t a = slp_addr(f, defs, base, block_start, depth + 1);
    if (!a.ok || (ny_i128)a.offset + k < INT64_MIN ||
        (ny_i128)a.offset + k > INT64_MAX)
      return bad;
    a.offset += k;
    return a;
  }
  return (addr_form_t){.ok = true, .root = value, .offset = 0};
}

static bool slp_reduce_quad_disabled(void) {
  static int disabled = -1;
  if (disabled < 0) {
    const char *env = getenv("NYTRIX_NO_SLP_REDUCE_QUAD_I64");
    disabled = env && *env && strcmp(env, "0") != 0;
  }
  return disabled != 0;
}

static int slp_unwrap_copy(const nyr_func_t *f, const int *defs, int v) {
  for (int depth = 0; depth < 8 && v >= 0 && v < f->next_value; ++depth) {
    int di = defs[v];
    if (di < 0 || (size_t)di >= f->len || f->data[(size_t)di].op != NYIR_COPY ||
        f->data[(size_t)di].a < 0)
      break;
    v = f->data[(size_t)di].a;
  }
  return v;
}

static bool slp_same_root(const nyr_func_t *f, const int *defs, int r0,
                          int r1) {
  r0 = slp_unwrap_copy(f, defs, r0);
  r1 = slp_unwrap_copy(f, defs, r1);
  if (r0 == r1)
    return true;
  if (slp_reduce_quad_disabled())
    return false;
  if (r0 < 0 || r1 < 0 || r0 >= f->next_value || r1 >= f->next_value)
    return false;
  int d0 = defs[r0], d1 = defs[r1];
  if (d0 < 0 || d1 < 0 || (size_t)d0 >= f->len || (size_t)d1 >= f->len)
    return false;
  const nyr_inst_t *i0 = &f->data[(size_t)d0];
  const nyr_inst_t *i1 = &f->data[(size_t)d1];
  if (i0->op != NYIR_ADD_I64 || i1->op != NYIR_ADD_I64)
    return false;
  int a0 = slp_unwrap_copy(f, defs, i0->a);
  int b0 = slp_unwrap_copy(f, defs, i0->b);
  int a1 = slp_unwrap_copy(f, defs, i1->a);
  int b1 = slp_unwrap_copy(f, defs, i1->b);
  return (a0 == a1 && b0 == b1) || (a0 == b1 && b0 == a1);
}

static bool slp_contiguous(const nyr_func_t *f, const int *defs, int a0, int a1,
                           size_t block_start) {
  addr_form_t x = slp_addr(f, defs, a0, block_start, 0);
  addr_form_t y = slp_addr(f, defs, a1, block_start, 0);
  return x.ok && y.ok && slp_same_root(f, defs, x.root, y.root) &&
         (ny_i128)y.offset - (ny_i128)x.offset == 8;
}

static bool slp_contiguous4(const nyr_func_t *f, const int *defs,
                            const int addr[4], size_t block_start) {
  addr_form_t a[4];
  for (size_t lane = 0; lane < 4; ++lane) {
    a[lane] = slp_addr(f, defs, addr[lane], block_start, 0);
    if (!a[lane].ok || !slp_same_root(f, defs, a[lane].root, a[0].root))
      return false;
  }
  for (size_t lane = 1; lane < 4; ++lane)
    if ((ny_i128)a[lane].offset - (ny_i128)a[0].offset != (ny_i128)lane * 8)
      return false;
  return true;
}

static bool slp_value_only_used_at(const nyr_use_def_t *uses, int value,
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

static bool slp_no_observable_between(const nyr_func_t *f, size_t a, size_t b) {
  if (!f)
    return false;
  if (a > b) {
    size_t t = a;
    a = b;
    b = t;
  }
  uint32_t a_alias = f->data[a].alias_class;
  for (size_t i = a + 1; i < b; ++i) {
    const nyr_inst_t *in = &f->data[i];
    unsigned effects = in->effects | nyr_inst_effects(in);
    if (effects & NYIR_EFFECT_WRITE_MEMORY) {
      /*
       * Alias metadata disambiguation: disjoint memory store does not alias
       * load
       */
      if (a_alias != 0 && in->alias_class != 0 && in->alias_class != a_alias)
        continue;
      return false;
    }
    if (effects & (NYIR_EFFECT_CALL | NYIR_EFFECT_MAY_TRAP |
                   NYIR_EFFECT_VOLATILE | NYIR_EFFECT_UNKNOWN_SIDE_EFFECT))
      return false;
  }
  return true;
}

static bool slp_no_effect_between(const nyr_func_t *f, size_t a, size_t b) {
  if (!f)
    return false;
  if (a > b) {
    size_t t = a;
    a = b;
    b = t;
  }
  uint32_t a_alias = f->data[a].alias_class;
  for (size_t i = a + 1; i < b; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->op == NYIR_NOP)
      continue;
    if (in->op == NYIR_LOAD_I64) {
      /*
       * Disjoint read between stores is safe when alias classes are proven
       * distinct
       */
      if (a_alias != 0 && in->alias_class != 0 && in->alias_class != a_alias)
        continue;
      return false;
    }
    if ((in->effects | nyr_inst_effects(in)) != NYIR_EFFECT_NONE ||
        in->op == NYIR_BR || in->op == NYIR_BR_IF || in->op == NYIR_RET)
      return false;
  }
  return true;
}

/*
 * Pack values v0/v1.  expected0/1 are their sole lane users in the original
 * use-def graph.  The packed result deliberately reuses v0's SSA number.
 */
static bool slp_pack_pair(nyr_func_t *f, const int *defs,
                          const nyr_use_def_t *uses, int v0, int v1,
                          size_t expected0, size_t expected1,
                          size_t block_start, bool f64, unsigned depth,
                          int *out_vec) {
  if (!f || !defs || !uses || !out_vec || depth > 12 || v0 < 0 || v1 < 0 ||
      v0 >= f->next_value || v1 >= f->next_value || defs[v0] < 0 ||
      defs[v1] < 0)
    return false;
  if (!slp_value_only_used_at(uses, v0, expected0) ||
      !slp_value_only_used_at(uses, v1, expected1))
    return false;

  size_t d0 = (size_t)defs[v0], d1 = (size_t)defs[v1];
  if (d0 < block_start || d1 < block_start)
    return false;
  nyr_inst_t i0 = f->data[d0];
  nyr_inst_t i1 = f->data[d1];
  if (i0.op != i1.op)
    return false;

  if (i0.op == NYIR_LOAD_I64) {
    bool l0_f64 = i0.scalar_class == NYIR_SCALAR_F64;
    bool l1_f64 = i1.scalar_class == NYIR_SCALAR_F64;
    if (l0_f64 != f64 || l1_f64 != f64 ||
        !slp_contiguous(f, defs, i0.a, i1.a, block_start) ||
        !slp_no_observable_between(f, d0, d1))
      return false;
    size_t keep = d0 > d1 ? d0 : d1;
    size_t kill = d0 > d1 ? d1 : d0;
    /*
     * Lane zero is always the first address, independent of instruction order.
     */
    slp_discard(f, kill);
    slp_assign(f, keep,
        (nyr_inst_t){.op = f64 ? NYIR_VEC4_LOAD_F64 : NYIR_VEC4_LOAD_I64,
                     .dst = v0,
                     .a = i0.a,
                     .b = -1,
                     .c = -1,
                     .d = -1,
                     .e = -1,
                     .f = -1,
                     .alias_class = i0.alias_class,
                     /*
                      * Keep a source anchor on the packed
                      * operation for debugger and IR diagnostics.
                      * Use the earliest scalar load in program
                      * order, including when lane order differs.
                      */
                     .debug = i0.debug,
                     .effects = NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_MAY_TRAP});
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
      int t = i0.a;
      i0.a = i0.b;
      i0.b = t;
    }
    if (slp_f64_broadcastable(f, defs, i1.a) &&
        !slp_f64_broadcastable(f, defs, i1.b)) {
      int t = i1.a;
      i1.a = i1.b;
      i1.b = t;
    }
  }

  bool shared_f64_rhs =
      f64 && i0.b == i1.b && slp_f64_broadcastable(f, defs, i0.b);
  int va = -1, vb = -1;
  bool first_ok = slp_pack_pair(f, defs, uses, i0.a, i1.a, d0, d1, block_start,
                                f64, depth + 1, &va);
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
      second_ok = slp_pack_pair(f, defs, uses, i0.b, i1.b, d0, d1, block_start,
                                f64, depth + 1, &vb);
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
    slp_assign(f, kill, (nyr_inst_t){.op = NYIR_VEC4_SET1_F64,
                                 .dst = vb,
                                 .a = i0.b,
                                 .b = -1,
                                 .c = -1,
                                 .d = -1,
                                 .e = -1,
                                 .f = -1,
                                 .debug = i0.debug,
                                 .effects = NYIR_EFFECT_NONE});
  } else {
    slp_discard(f, kill);
  }
  slp_assign(f, keep, (nyr_inst_t){.op = slp_vec_binop(i0.op),
                               .dst = v0,
                               .a = va,
                               .b = vb,
                               .c = -1,
                               .d = -1,
                               .e = -1,
                               .f = -1,
                               .debug = i0.debug,
                               .effects = NYIR_EFFECT_NONE});
  *out_vec = v0;
  return true;
}

static bool slp_quad_load_group_safe(const nyr_func_t *f, const size_t at[4]) {
  if (!f || !at)
    return false;
  size_t lo = at[0], hi = at[0];
  for (size_t lane = 1; lane < 4; ++lane) {
    if (at[lane] < lo)
      lo = at[lane];
    if (at[lane] > hi)
      hi = at[lane];
  }
  for (size_t i = lo; i <= hi; ++i) {
    bool lane_load = false;
    for (size_t lane = 0; lane < 4; ++lane)
      if (i == at[lane]) {
        lane_load = true;
        break;
      }
    if (lane_load || f->data[i].op == NYIR_NOP)
      continue;
    if (!slp_reduce_quad_disabled()) {
      const nyr_inst_t *in = &f->data[i];
      unsigned eff = in->effects | nyr_inst_effects(in);
      if (eff == NYIR_EFFECT_NONE && in->op != NYIR_BR &&
          in->op != NYIR_BR_IF && in->op != NYIR_RET &&
          in->op != NYIR_LABEL)
        continue;
    }
    /*
     * Do not move/widen a potentially trapping load across unrelated work.
     */
    return false;
  }
  return true;
}

static bool slp_pack_quad_load_i64(nyr_func_t *f, const int *defs,
                                   const nyr_use_def_t *uses,
                                   const int value[4], const size_t user[4],
                                   size_t block_start, int *out_vec) {
  size_t at[4];
  int addr[4];
  for (size_t lane = 0; lane < 4; ++lane) {
    if (value[lane] < 0 || value[lane] >= f->next_value ||
        defs[value[lane]] < 0 ||
        !slp_value_only_used_at(uses, value[lane], user[lane]))
      return false;
    at[lane] = (size_t)defs[value[lane]];
    if (at[lane] < block_start || f->data[at[lane]].op != NYIR_LOAD_I64 ||
        f->data[at[lane]].scalar_class == NYIR_SCALAR_F64)
      return false;
    addr[lane] = f->data[at[lane]].a;
  }
  if (!slp_contiguous4(f, defs, addr, block_start))
    return false;
  size_t lo = at[0], hi = at[0];
  for (size_t lane = 1; lane < 4; ++lane) {
    if (at[lane] < lo)
      lo = at[lane];
    if (at[lane] > hi)
      hi = at[lane];
  }
  if (!slp_quad_load_group_safe(f, at))
    return false;
  nyr_debug_loc_t debug = f->data[lo].debug;
  for (size_t lane = 0; lane < 4; ++lane)
    if (at[lane] != hi)
      slp_discard(f, at[lane]);
  slp_assign(f, hi,
      (nyr_inst_t){.op = NYIR_VEC8_LOAD_I64,
                   .dst = value[0],
                   .a = addr[0],
                   .b = -1,
                   .c = -1,
                   .d = -1,
                   .e = -1,
                   .f = -1,
                   .debug = debug,
                   .effects = NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_MAY_TRAP});
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
static bool slp_try_store_quad_i64(nyr_func_t *f, const size_t store[4],
                                   size_t block_start, const int *defs,
                                   const nyr_use_def_t *uses) {
  if (!f || !defs || !uses)
    return false;
  bool ok = true;
  int store_addr[4], result[4], lhs[4], rhs[4];
  size_t bin_at[4];
  nyr_op_t scalar_op = NYIR_NOP;
  size_t min_bin = SIZE_MAX, max_bin = 0;
  for (size_t lane = 0; lane < 4 && ok; ++lane) {
    if (store[lane] >= f->len) {
      ok = false;
      break;
    }
    const nyr_inst_t *st = &f->data[store[lane]];
    if (st->op != NYIR_STORE_I64 || st->scalar_class == NYIR_SCALAR_F64 ||
        st->c < 0) {
      ok = false;
      break;
    }
    store_addr[lane] = st->a;
    result[lane] = st->c;
    if (result[lane] >= f->next_value || defs[result[lane]] < 0 ||
        !slp_value_only_used_at(uses, result[lane], store[lane])) {
      ok = false;
      break;
    }
    bin_at[lane] = (size_t)defs[result[lane]];
    const nyr_inst_t *bin = &f->data[bin_at[lane]];
    if (!slp_binop(bin->op, false) || slp_vec8_i64_binop(bin->op) == NYIR_NOP ||
        bin->a < 0 || bin->b < 0) {
      ok = false;
      break;
    }
    if (lane == 0)
      scalar_op = bin->op;
    else if (bin->op != scalar_op) {
      ok = false;
      break;
    }
    lhs[lane] = bin->a;
    rhs[lane] = bin->b;
    if (bin_at[lane] < min_bin)
      min_bin = bin_at[lane];
    if (bin_at[lane] > max_bin)
      max_bin = bin_at[lane];
  }
  if (ok && !slp_contiguous4(f, defs, store_addr, block_start))
    ok = false;
  nyr_debug_loc_t store_debug = f->data[store[0]].debug;
  if (ok) {
    for (size_t lane = 0; lane < 4; ++lane)
      if (bin_at[lane] >= store[lane]) {
        ok = false;
        break;
      }
  }

  int vlhs = -1, vrhs = -1;
  bool same_operands = true;
  for (size_t lane = 0; lane < 4; ++lane)
    if (lhs[lane] != rhs[lane]) {
      same_operands = false;
      break;
    }
  size_t lhs_max_def = 0, rhs_max_def = 0;
  if (ok) {
    for (size_t lane = 0; lane < 4; ++lane) {
      if (lhs[lane] < 0 || lhs[lane] >= f->next_value || defs[lhs[lane]] < 0) {
        ok = false;
        break;
      }
      size_t di = (size_t)defs[lhs[lane]];
      if (di > lhs_max_def)
        lhs_max_def = di;
    }
    if (lhs_max_def >= min_bin)
      ok = false;
  }
  if (ok && !same_operands) {
    for (size_t lane = 0; lane < 4; ++lane) {
      if (rhs[lane] < 0 || rhs[lane] >= f->next_value || defs[rhs[lane]] < 0) {
        ok = false;
        break;
      }
      size_t di = (size_t)defs[rhs[lane]];
      if (di > rhs_max_def)
        rhs_max_def = di;
    }
    if (rhs_max_def >= min_bin)
      ok = false;
  }

  if (ok)
    ok = slp_pack_quad_load_i64(f, defs, uses, lhs, bin_at, block_start, &vlhs);
  if (ok) {
    if (same_operands) {
      vrhs = vlhs;
    } else {
      ok = slp_pack_quad_load_i64(f, defs, uses, rhs, bin_at, block_start,
                                  &vrhs);
    }
  }
  if (ok) {
    nyr_debug_loc_t bin_debug = f->data[min_bin].debug;
    for (size_t lane = 0; lane < 4; ++lane)
      if (bin_at[lane] != max_bin)
        slp_discard(f, bin_at[lane]);
    slp_assign(f, max_bin, (nyr_inst_t){.op = slp_vec8_i64_binop(scalar_op),
                                    .dst = result[0],
                                    .a = vlhs,
                                    .b = vrhs,
                                    .c = -1,
                                    .d = -1,
                                    .e = -1,
                                    .f = -1,
                                    .debug = bin_debug,
                                    .effects = NYIR_EFFECT_NONE});
    size_t keep = store[3];
    for (size_t lane = 0; lane < 3; ++lane)
      slp_discard(f, store[lane]);
    slp_assign(f, keep, (nyr_inst_t){.op = NYIR_VEC8_STORE_I64,
                                 .dst = -1,
                                 .a = store_addr[0],
                                 .b = result[0],
                                 .c = -1,
                                 .d = -1,
                                 .e = -1,
                                 .f = -1,
                                 .debug = store_debug,
                                 .effects = NYIR_EFFECT_WRITE_MEMORY |
                                            NYIR_EFFECT_MAY_TRAP});
  }
  return ok;
}

static bool slp_try_store_pair(nyr_func_t *f, size_t s0, size_t s1,
                               size_t block_start, const int *defs,
                               const nyr_use_def_t *uses) {
  if (!f || !defs || !uses || s0 >= f->len || s1 >= f->len || s0 == s1)
    return false;
  const nyr_inst_t st0 = f->data[s0], st1 = f->data[s1];
  if (st0.op != NYIR_STORE_I64 || st1.op != NYIR_STORE_I64 || st0.c < 0 ||
      st1.c < 0 || !slp_no_effect_between(f, s0, s1))
    return false;

  bool st0_f64 = st0.scalar_class == NYIR_SCALAR_F64;
  bool st1_f64 = st1.scalar_class == NYIR_SCALAR_F64;
  bool f64 = st0_f64 && st1_f64;
  bool ok =
      st0_f64 == st1_f64 && slp_contiguous(f, defs, st0.a, st1.a, block_start);
  int packed = -1;
  if (ok)
    ok = slp_pack_pair(f, defs, uses, st0.c, st1.c, s0, s1, block_start, f64, 0,
                       &packed);
  if (ok) {
    size_t keep = s0 > s1 ? s0 : s1;
    size_t kill = s0 > s1 ? s1 : s0;
    slp_discard(f, kill);
    slp_assign(f, keep, (nyr_inst_t){
        .op = f64 ? NYIR_VEC4_STORE_F64 : NYIR_VEC4_STORE_I64,
        .dst = -1,
        .a = st0.a,
        .b = packed,
        .c = -1,
        .d = -1,
        .e = -1,
        .f = -1,
        .alias_class = st0.alias_class,
        .debug = st0.debug,
        .effects = NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP});
  }
  return ok;
}

/*
 * Find the next scalar store while allowing only movable pure work in
 * between.  Delaying the first store to the second store's position is safe
 * across such instructions; any memory, trap, call, volatile, or control
 * effect terminates the search unless alias metadata proves disjointness.
 */
static int slp_alloc_root(const nyr_func_t *f, const int *defs, int v,
                          int active_phi, unsigned depth) {
  if (!f || !defs || depth > 16 || v < 0 || v >= f->next_value)
    return -1;
  if (v == active_phi)
    return -2;
  int di = defs[v];
  if (di < 0 || (size_t)di >= f->len)
    return -1;
  const nyr_inst_t *in = &f->data[(size_t)di];
  if (in->op == NYIR_ALLOCA || in->op == NYIR_ADDR_LOCAL)
    return di;
  if (in->op == NYIR_CALL && in->symbol &&
      (strcmp(in->symbol, "malloc") == 0 || strcmp(in->symbol, "calloc") == 0 ||
       strcmp(in->symbol, "rt_malloc_i64") == 0 ||
       strcmp(in->symbol, "rt_calloc_i64") == 0 ||
       strcmp(in->symbol, "f64buf_new") == 0 ||
       strcmp(in->symbol, "i64buf_new") == 0))
    return di;
  if (in->op == NYIR_COPY || in->op == NYIR_SUB_I64)
    return slp_alloc_root(f, defs, in->a, active_phi, depth + 1);
  if (in->op == NYIR_ADD_I64) {
    int ra = slp_alloc_root(f, defs, in->a, active_phi, depth + 1);
    int rb = slp_alloc_root(f, defs, in->b, active_phi, depth + 1);
    if (ra == -2 && rb == -1)
      return -2;
    if (rb == -2 && ra == -1)
      return -2;
    if (ra >= 0 && rb == -1)
      return ra;
    if (rb >= 0 && ra == -1)
      return rb;
    return -1;
  }
  if (in->op == NYIR_PHI && active_phi < 0 && in->phi_incoming &&
      in->phi_incoming_len > 0) {
    int root = -1;
    for (size_t k = 0; k < in->phi_incoming_len; ++k) {
      int rk =
          slp_alloc_root(f, defs, in->phi_incoming[k].value, v, depth + 1);
      if (rk == -2)
        continue;
      if (rk < 0)
        return -1;
      if (root < 0)
        root = rk;
      else if (root != rk)
        return -1;
    }
    return root;
  }
  return -1;
}

static bool slp_addrs_disjoint(const nyr_func_t *f, const int *defs,
                               size_t st_pc, size_t ld_pc, size_t block_start) {
  if (!f || !defs || st_pc >= f->len || ld_pc >= f->len)
    return false;
  const nyr_inst_t *st = &f->data[st_pc];
  const nyr_inst_t *ld = &f->data[ld_pc];
  if (st->alias_class != 0 && ld->alias_class != 0 &&
      st->alias_class != ld->alias_class)
    return true;
  unsigned narrow =
      NYIR_INST_F_MEM_BYTE | NYIR_INST_F_MEM_U16 | NYIR_INST_F_MEM_I32;
  if ((st->flags & narrow) || (ld->flags & narrow))
    return false;
  addr_form_t sa = slp_addr(f, defs, st->a, block_start, 0);
  addr_form_t la = slp_addr(f, defs, ld->a, block_start, 0);
  if (!sa.ok || !la.ok)
    return false;
  if (sa.root == la.root) {
    ny_i128 d = (ny_i128)sa.offset - (ny_i128)la.offset;
    return d >= 8 || d <= -8;
  }
  int sr = slp_alloc_root(f, defs, sa.root, -1, 0);
  int lr = slp_alloc_root(f, defs, la.root, -1, 0);
  return sr >= 0 && lr >= 0 && sr != lr;
}

static bool slp_pack_quad_f64(nyr_func_t *f, const int *defs,
                              const nyr_use_def_t *uses, const int val[4],
                              const size_t expected[4], const size_t stores[4],
                              size_t block_start, unsigned depth, int *out_vec,
                              size_t *out_pos) {
  if (!f || !defs || !uses || !out_vec || !out_pos || depth > 8)
    return false;
  size_t at[4];
  nyr_op_t op = NYIR_NOP;
  size_t lo = SIZE_MAX, hi = 0;
  for (size_t lane = 0; lane < 4; ++lane) {
    if (val[lane] < 0 || val[lane] >= f->next_value || defs[val[lane]] < 0 ||
        !slp_value_only_used_at(uses, val[lane], expected[lane]))
      return false;
    at[lane] = (size_t)defs[val[lane]];
    if (at[lane] < block_start || at[lane] >= f->len)
      return false;
    const nyr_inst_t *in = &f->data[at[lane]];
    if (lane == 0)
      op = in->op;
    else if (in->op != op)
      return false;
    if (at[lane] < lo)
      lo = at[lane];
    if (at[lane] > hi)
      hi = at[lane];
  }
  if (op == NYIR_LOAD_I64) {
    int addr[4];
    unsigned narrow =
        NYIR_INST_F_MEM_BYTE | NYIR_INST_F_MEM_U16 | NYIR_INST_F_MEM_I32;
    for (size_t lane = 0; lane < 4; ++lane) {
      const nyr_inst_t *ld = &f->data[at[lane]];
      if (ld->scalar_class != NYIR_SCALAR_F64 || (ld->flags & narrow) ||
          (ld->effects & NYIR_EFFECT_VOLATILE))
        return false;
      addr[lane] = ld->a;
    }
    if (!slp_contiguous4(f, defs, addr, block_start))
      return false;
    for (size_t k = lo; k <= hi; ++k) {
      bool is_self = false;
      for (size_t lane = 0; lane < 4; ++lane)
        if (k == at[lane] || k == stores[lane]) {
          is_self = true;
          break;
        }
      if (is_self || f->data[k].op == NYIR_NOP ||
          f->data[k].op == NYIR_LOAD_I64 ||
          f->data[k].op == NYIR_VEC4_LOAD_F64)
        continue;
      unsigned eff = f->data[k].effects | nyr_inst_effects(&f->data[k]);
      if (eff != NYIR_EFFECT_NONE || f->data[k].op == NYIR_BR ||
          f->data[k].op == NYIR_BR_IF || f->data[k].op == NYIR_RET)
        return false;
    }
    size_t keep = at[0];
    if (defs[addr[0]] >= 0 && (size_t)defs[addr[0]] >= keep)
      return false;
    nyr_inst_t l0 = f->data[at[0]];
    for (size_t lane = 1; lane < 4; ++lane)
      slp_discard(f, at[lane]);
    slp_assign(
        f, keep,
        (nyr_inst_t){.op = NYIR_VEC4_LOAD_F64,
                     .dst = val[0],
                     .a = addr[0],
                     .b = -1,
                     .c = -1,
                     .d = -1,
                     .e = -1,
                     .f = -1,
                     .alias_class = l0.alias_class,
                     .debug = l0.debug,
                     .effects = NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_MAY_TRAP});
    *out_vec = val[0];
    *out_pos = keep;
    return true;
  }
  if (!slp_binop(op, true))
    return false;
  int lhs[4], rhs[4];
  for (size_t lane = 0; lane < 4; ++lane) {
    const nyr_inst_t *in = &f->data[at[lane]];
    if (in->a < 0 || in->b < 0)
      return false;
    lhs[lane] = in->a;
    rhs[lane] = in->b;
  }
  bool shared_lhs =
      lhs[0] == lhs[1] && lhs[0] == lhs[2] && lhs[0] == lhs[3] &&
      (defs[lhs[0]] < 0 || (size_t)defs[lhs[0]] < block_start ||
       (f->data[(size_t)defs[lhs[0]]].op == NYIR_CONST_F64 &&
        (size_t)defs[lhs[0]] < lo));
  bool shared_rhs =
      rhs[0] == rhs[1] && rhs[0] == rhs[2] && rhs[0] == rhs[3] &&
      (defs[rhs[0]] < 0 || (size_t)defs[rhs[0]] < block_start ||
       (f->data[(size_t)defs[rhs[0]]].op == NYIR_CONST_F64 &&
        (size_t)defs[rhs[0]] < lo));
  if (shared_lhs && shared_rhs)
    return false;
  int va = -1, vb = -1;
  size_t pos_a = 0, pos_b = 0;
  size_t bc_slot = SIZE_MAX;
  int bc_src = -1;
  if (shared_lhs) {
    if (lo >= hi)
      return false;
    va = f->next_value++;
    bc_slot = lo;
    bc_src = lhs[0];
    pos_a = lo;
    if (!slp_pack_quad_f64(f, defs, uses, rhs, at, stores, block_start,
                           depth + 1, &vb, &pos_b))
      return false;
  } else if (shared_rhs) {
    if (lo >= hi)
      return false;
    if (!slp_pack_quad_f64(f, defs, uses, lhs, at, stores, block_start,
                           depth + 1, &va, &pos_a))
      return false;
    vb = f->next_value++;
    bc_slot = lo;
    bc_src = rhs[0];
    pos_b = lo;
  } else {
    if (!slp_pack_quad_f64(f, defs, uses, lhs, at, stores, block_start,
                           depth + 1, &va, &pos_a))
      return false;
    bool same_ops = lhs[0] == rhs[0] && lhs[1] == rhs[1] && lhs[2] == rhs[2] &&
                    lhs[3] == rhs[3];
    if (same_ops) {
      vb = va;
      pos_b = pos_a;
    } else if (!slp_pack_quad_f64(f, defs, uses, rhs, at, stores, block_start,
                                  depth + 1, &vb, &pos_b)) {
      return false;
    }
  }
  if (pos_a >= hi || pos_b >= hi)
    return false;
  nyr_debug_loc_t dbg = f->data[lo].debug;
  for (size_t lane = 0; lane < 4; ++lane) {
    if (at[lane] == hi)
      continue;
    if (at[lane] == bc_slot)
      continue;
    slp_discard(f, at[lane]);
  }
  if (bc_slot != SIZE_MAX) {
    slp_assign(f, bc_slot,
               (nyr_inst_t){.op = NYIR_VEC4_SET1_F64,
                            .dst = shared_lhs ? va : vb,
                            .a = bc_src,
                            .b = -1,
                            .c = -1,
                            .d = -1,
                            .e = -1,
                            .f = -1,
                            .debug = dbg,
                            .effects = NYIR_EFFECT_NONE});
  }
  slp_assign(f, hi,
             (nyr_inst_t){.op = slp_vec_binop(op),
                          .dst = val[0],
                          .a = va,
                          .b = vb,
                          .c = -1,
                          .d = -1,
                          .e = -1,
                          .f = -1,
                          .debug = dbg,
                          .effects = NYIR_EFFECT_NONE});
  *out_vec = val[0];
  *out_pos = hi;
  return true;
}

static bool slp_try_store_quad_f64(nyr_func_t *f, const size_t store[4],
                                   size_t block_start, const int *defs,
                                   const nyr_use_def_t *uses) {
  if (!f || !defs || !uses)
    return false;
  int store_addr[4], store_val[4];
  unsigned narrow =
      NYIR_INST_F_MEM_BYTE | NYIR_INST_F_MEM_U16 | NYIR_INST_F_MEM_I32;
  for (size_t lane = 0; lane < 4; ++lane) {
    if (store[lane] >= f->len)
      return false;
    const nyr_inst_t *st = &f->data[store[lane]];
    if (st->op != NYIR_STORE_I64 || st->scalar_class != NYIR_SCALAR_F64 ||
        st->c < 0 || (st->flags & narrow) ||
        (st->effects & NYIR_EFFECT_VOLATILE))
      return false;
    store_addr[lane] = st->a;
    store_val[lane] = st->c;
  }
  if (!slp_contiguous4(f, defs, store_addr, block_start))
    return false;
  if (defs[store_addr[0]] >= 0 && (size_t)defs[store_addr[0]] >= store[3])
    return false;
  nyr_inst_t s0 = f->data[store[0]];
  int packed = -1;
  size_t packed_pos = 0;
  if (!slp_pack_quad_f64(f, defs, uses, store_val, store, store, block_start, 0,
                         &packed, &packed_pos))
    return false;
  if (packed_pos >= store[3])
    return false;
  for (size_t lane = 0; lane < 3; ++lane)
    slp_discard(f, store[lane]);
  slp_assign(f, store[3],
             (nyr_inst_t){.op = NYIR_VEC4_STORE_F64,
                          .dst = -1,
                          .a = store_addr[0],
                          .b = packed,
                          .c = -1,
                          .d = -1,
                          .e = -1,
                          .f = -1,
                          .alias_class = s0.alias_class,
                          .debug = s0.debug,
                          .effects =
                              NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP});
  return true;
}

static size_t slp_next_store_in_block(const nyr_func_t *f, const int *defs,
                                      size_t block_start,
                                      const size_t *prior_stores,
                                      size_t prior_count, size_t at,
                                      size_t end) {
  for (++at; at < end; ++at) {
    const nyr_inst_t *in = &f->data[at];
    if (in->op == NYIR_NOP)
      continue;
    if (in->op == NYIR_STORE_I64)
      return at;
    if (in->op == NYIR_LOAD_I64) {
      bool disjoint_all = prior_count > 0;
      for (size_t p = 0; disjoint_all && p < prior_count; ++p)
        disjoint_all =
            slp_addrs_disjoint(f, defs, prior_stores[p], at, block_start);
      if (disjoint_all)
        continue;
      return end;
    }
    if ((in->effects | nyr_inst_effects(in)) != NYIR_EFFECT_NONE ||
        in->op == NYIR_BR || in->op == NYIR_BR_IF || in->op == NYIR_RET ||
        in->op == NYIR_LABEL)
      return end;
  }
  return end;
}

/*
 * Detect straight-line 4-lane horizontal i64 add reduction trees rooted in one
 * basic block so interleaved load+add reductions vectorize to AVX2
 * VEC8_LOAD_I64 + VEC8_ADD_I64 + VEC8_REDUCE_ADD_I64.
 */
static bool slp_has_add_quad_cand(const nyr_func_t *f) {
  unsigned loads = 0, adds = 0;
  for (size_t i = 0; i < f->len; ++i) {
    nyr_op_t op = f->data[i].op;
    if (op == NYIR_LABEL || op == NYIR_BR || op == NYIR_BR_IF ||
        op == NYIR_RET) {
      loads = 0;
      adds = 0;
    } else if (op == NYIR_LOAD_I64) {
      ++loads;
      if (loads >= 4 && adds >= 3)
        return true;
    } else if (op == NYIR_ADD_I64) {
      ++adds;
      if (loads >= 4 && adds >= 3)
        return true;
    }
  }
  return false;
}

static bool slp_pack_add_quad_i64(nyr_func_t *f, const int *defs,
                                  const nyr_use_def_t *uses, const int val[4],
                                  const size_t expected[4], size_t block_start,
                                  unsigned depth, int *out_vec,
                                  size_t *out_pos) {
  if (!f || !defs || !uses || !out_vec || !out_pos || depth > 4)
    return false;
  size_t at[4];
  nyr_op_t op = NYIR_NOP;
  size_t hi = 0;
  for (size_t lane = 0; lane < 4; ++lane) {
    if (val[lane] < 0 || val[lane] >= f->next_value)
      return false;
    for (size_t prev = 0; prev < lane; ++prev)
      if (val[prev] == val[lane])
        return false;
    int di = defs[val[lane]];
    if (di < 0 || (size_t)di < block_start || (size_t)di >= f->len)
      return false;
    at[lane] = (size_t)di;
    if (!slp_value_only_used_at(uses, val[lane], expected[lane]))
      return false;
    const nyr_inst_t *in = &f->data[at[lane]];
    if (in->scalar_class == NYIR_SCALAR_F64)
      return false;
    if (lane == 0)
      op = in->op;
    else if (in->op != op)
      return false;
    if (at[lane] > hi)
      hi = at[lane];
  }
  if (op == NYIR_LOAD_I64) {
    unsigned narrow =
        NYIR_INST_F_MEM_BYTE | NYIR_INST_F_MEM_U16 | NYIR_INST_F_MEM_I32;
    for (size_t lane = 0; lane < 4; ++lane) {
      const nyr_inst_t *ld = &f->data[at[lane]];
      if ((ld->flags & narrow) || (ld->effects & NYIR_EFFECT_VOLATILE))
        return false;
    }
    if (!slp_pack_quad_load_i64(f, defs, uses, val, expected, block_start,
                                out_vec))
      return false;
    *out_pos = hi;
    return true;
  }
  if (op != NYIR_ADD_I64)
    return false;
  int lhs[4], rhs[4];
  for (size_t lane = 0; lane < 4; ++lane) {
    const nyr_inst_t *in = &f->data[at[lane]];
    if (in->a < 0 || in->b < 0 || in->a == in->b)
      return false;
    lhs[lane] = in->a;
    rhs[lane] = in->b;
  }
  nyr_op_t want_lhs = NYIR_NOP;
  if (lhs[0] < f->next_value && defs[lhs[0]] >= (int)block_start &&
      (size_t)defs[lhs[0]] < f->len)
    want_lhs = f->data[(size_t)defs[lhs[0]]].op;
  for (size_t lane = 1; lane < 4; ++lane) {
    nyr_op_t lop = (lhs[lane] < f->next_value &&
                    defs[lhs[lane]] >= (int)block_start &&
                    (size_t)defs[lhs[lane]] < f->len)
                       ? f->data[(size_t)defs[lhs[lane]]].op
                       : NYIR_NOP;
    if (want_lhs != NYIR_NOP && lop != want_lhs) {
      nyr_op_t rop = (rhs[lane] < f->next_value &&
                      defs[rhs[lane]] >= (int)block_start &&
                      (size_t)defs[rhs[lane]] < f->len)
                         ? f->data[(size_t)defs[rhs[lane]]].op
                         : NYIR_NOP;
      if (rop == want_lhs) {
        int tmp = lhs[lane];
        lhs[lane] = rhs[lane];
        rhs[lane] = tmp;
      }
    }
  }
  int vlhs = -1, vrhs = -1;
  size_t lhs_pos = 0, rhs_pos = 0;
  if (!slp_pack_add_quad_i64(f, defs, uses, lhs, at, block_start, depth + 1,
                             &vlhs, &lhs_pos))
    return false;
  if (!slp_pack_add_quad_i64(f, defs, uses, rhs, at, block_start, depth + 1,
                             &vrhs, &rhs_pos))
    return false;
  if (lhs_pos >= hi || rhs_pos >= hi)
    return false;
  nyr_debug_loc_t debug = f->data[hi].debug;
  for (size_t lane = 0; lane < 4; ++lane)
    if (at[lane] != hi)
      slp_discard(f, at[lane]);
  slp_assign(f, hi,
             (nyr_inst_t){.op = NYIR_VEC8_ADD_I64,
                          .dst = val[0],
                          .a = vlhs,
                          .b = vrhs,
                          .c = -1,
                          .d = -1,
                          .e = -1,
                          .f = -1,
                          .debug = debug,
                          .effects = NYIR_EFFECT_NONE});
  *out_vec = val[0];
  *out_pos = hi;
  return true;
}

static bool slp_try_reduce_quad_i64(nyr_func_t *f, size_t root_pc,
                                    size_t block_start, const int *defs,
                                    const nyr_use_def_t *uses,
                                    bool swap_pairs) {
  if (!f || !defs || !uses || root_pc < block_start || root_pc >= f->len)
    return false;
  nyr_inst_t root = f->data[root_pc];
  if (root.op != NYIR_ADD_I64 || root.dst < 0 ||
      root.scalar_class == NYIR_SCALAR_F64 || root.a < 0 || root.b < 0 ||
      root.a == root.b || root.a >= f->next_value || root.b >= f->next_value)
    return false;
  int p0_v = swap_pairs ? root.b : root.a;
  int p1_v = swap_pairs ? root.a : root.b;
  if (defs[p0_v] < (int)block_start || defs[p1_v] < (int)block_start)
    return false;
  size_t p0_pc = (size_t)defs[p0_v];
  size_t p1_pc = (size_t)defs[p1_v];
  if (p0_pc >= root_pc || p1_pc >= root_pc || p0_pc == p1_pc)
    return false;
  if (!slp_value_only_used_at(uses, p0_v, root_pc) ||
      !slp_value_only_used_at(uses, p1_v, root_pc))
    return false;
  nyr_inst_t p0 = f->data[p0_pc];
  nyr_inst_t p1 = f->data[p1_pc];
  if (p0.op != NYIR_ADD_I64 || p1.op != NYIR_ADD_I64 ||
      p0.scalar_class == NYIR_SCALAR_F64 || p1.scalar_class == NYIR_SCALAR_F64 ||
      p0.a < 0 || p0.b < 0 || p1.a < 0 || p1.b < 0)
    return false;
  int lanes[4] = {p0.a, p0.b, p1.a, p1.b};
  size_t expected[4] = {p0_pc, p0_pc, p1_pc, p1_pc};
  int vec = -1;
  size_t vec_pos = 0;
  if (!slp_pack_add_quad_i64(f, defs, uses, lanes, expected, block_start, 0,
                             &vec, &vec_pos))
    return false;
  if (vec_pos >= root_pc)
    return false;
  int zero_v = f->next_value++;
  slp_assign(f, p0_pc,
             (nyr_inst_t){.op = NYIR_CONST_I64,
                          .dst = zero_v,
                          .a = -1,
                          .b = -1,
                          .c = -1,
                          .d = -1,
                          .e = -1,
                          .f = -1,
                          .imm = 0,
                          .debug = root.debug,
                          .effects = NYIR_EFFECT_NONE});
  slp_discard(f, p1_pc);
  slp_assign(f, root_pc,
             (nyr_inst_t){.op = NYIR_VEC8_REDUCE_ADD_I64,
                          .dst = root.dst,
                          .a = zero_v,
                          .b = vec,
                          .c = -1,
                          .d = -1,
                          .e = -1,
                          .f = -1,
                          .scalar_class = root.scalar_class,
                          .debug = root.debug,
                          .effects = NYIR_EFFECT_NONE});
  return true;
}

/*
 * Every transform below starts with two stores in one basic block.
 */
static bool slp_has_store_pair(const nyr_func_t *f) {
  bool seen_store = false;
  for (size_t i = 0; i < f->len; ++i) {
    nyr_op_t op = f->data[i].op;
    if (op == NYIR_LABEL || op == NYIR_BR || op == NYIR_BR_IF ||
        op == NYIR_RET) {
      seen_store = false;
    } else if (op == NYIR_STORE_I64) {
      if (seen_store)
        return true;
      seen_store = true;
    }
  }
  return false;
}

bool nyr_slp_vectorize(nyr_func_t *f) {
  if (!f || f->len < 4 || f->next_value <= 0 ||
      (!slp_has_store_pair(f) &&
       (slp_reduce_quad_disabled() || !slp_has_add_quad_cand(f))))
    return true;

  nyr_func_t work = {0};
  if (!nyr_ir_func_clone(f, &work))
    return false;
  bool changed = false;
  slp_journal_t journal = {0};

  for (unsigned round = 0; round < 64; ++round) {
    nyr_cfg_t cfg = {0};
    if (!nyr_cfg_build(&work, &cfg)) {
      nyr_ir_func_free(&work);
      return false;
    }
    int *defs = nyr_build_defs(&work);
    nyr_use_def_t uses = {0};
    if (!defs || !nyr_build_use_def(&work, &uses)) {
      free(defs);
      nyr_cfg_free(&cfg);
      nyr_ir_func_free(&work);
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
          /*
           * The helper advances before inspecting; pass the previous store
           * itself so an immediately adjacent store remains a candidate.
           */
          stores[lane] = slp_next_store_in_block(
              &work, defs, start, stores, lane, stores[lane - 1], end);
          if (stores[lane] >= end ||
              work.data[stores[lane]].op != NYIR_STORE_I64)
            break;
        }
        bool quad_contiguous =
            stores[3] < end && work.data[stores[3]].op == NYIR_STORE_I64;
        for (size_t lane = 1; quad_contiguous && lane < 4; ++lane) {
          const nyr_inst_t *prev = &work.data[stores[lane - 1]];
          const nyr_inst_t *next = &work.data[stores[lane]];
          quad_contiguous =
              prev->scalar_class == next->scalar_class &&
              slp_contiguous(&work, defs, prev->a, next->a, start);
        }
        if (quad_contiguous) {
          bool quad_f64 = work.data[stores[0]].scalar_class == NYIR_SCALAR_F64;
          ny_dbg_heuristic_event("SLP",
                                 "candidate kind=quad block=%zu "
                                 "stores=%zu,%zu,%zu,%zu lanes=4 type=%s",
                                 b, stores[0], stores[1], stores[2], stores[3],
                                 quad_f64 ? "f64" : "i64");
          slp_journal_begin(&journal, &work);
          bool quad_ok =
              quad_f64
                  ? slp_try_store_quad_f64(&work, stores, start, defs, &uses)
                  : slp_try_store_quad_i64(&work, stores, start, defs, &uses);
          if (journal.failed) {
            slp_journal = NULL;
            free(journal.items);
            nyr_use_def_free(&uses);
            free(defs);
            nyr_cfg_free(&cfg);
            nyr_ir_func_free(&work);
            return false;
          }
          if (quad_ok) {
            char err[256] = {0};
            if (nyr_ir_verify(&work, err, sizeof(err))) {
              ny_dbg_heuristic_event("SLP",
                                     "decision=accept kind=quad block=%zu "
                                     "lanes=4 reason=pack-legal-and-verifies",
                                     b);
              if (!slp_accept(&journal, &work)) {
                free(journal.items);
                nyr_use_def_free(&uses);
                free(defs);
                nyr_cfg_free(&cfg);
                return false;
              }
              transformed = true;
              changed = true;
              break;
            } else {
              ny_dbg_heuristic_event("SLP",
                                     "decision=reject kind=quad block=%zu "
                                     "reason=verification detail=%s",
                                     b, err[0] ? err : "unknown");
            }
          } else {
            ny_dbg_heuristic_event("SLP",
                                   "decision=reject kind=quad block=%zu "
                                   "reason=tree-or-use-legality",
                                   b);
          }
          slp_journal_rollback(&journal, &work);
        }
        size_t j = slp_next_store_in_block(&work, defs, start, stores, 1, i, end);
        if (j >= end || work.data[j].op != NYIR_STORE_I64)
          continue;
        if (work.data[i].scalar_class != work.data[j].scalar_class ||
            !slp_contiguous(&work, defs, work.data[i].a, work.data[j].a, start))
          continue;

        ny_dbg_heuristic_event(
            "SLP",
            "candidate kind=pair block=%zu stores=%zu,%zu lanes=2 type=i64", b,
            i, j);

        slp_journal_begin(&journal, &work);
        bool pair_ok = slp_try_store_pair(&work, i, j, start, defs, &uses);
        if (journal.failed) {
          slp_journal = NULL;
          free(journal.items);
          nyr_use_def_free(&uses);
          free(defs);
          nyr_cfg_free(&cfg);
          nyr_ir_func_free(&work);
          return false;
        }
        if (pair_ok) {
          char err[256] = {0};
          if (nyr_ir_verify(&work, err, sizeof(err))) {
            ny_dbg_heuristic_event("SLP",
                                   "decision=accept kind=pair block=%zu "
                                   "lanes=2 reason=pack-legal-and-verifies",
                                   b);
            if (!slp_accept(&journal, &work)) {
              free(journal.items);
              nyr_use_def_free(&uses);
              free(defs);
              nyr_cfg_free(&cfg);
              return false;
            }
            transformed = true;
            changed = true;
          } else {
            ny_dbg_heuristic_event("SLP",
                                   "decision=reject kind=pair block=%zu "
                                   "reason=verification detail=%s",
                                   b, err[0] ? err : "unknown");
            slp_journal_rollback(&journal, &work);
          }
        } else {
          ny_dbg_heuristic_event(
              "SLP",
              "decision=reject kind=pair block=%zu reason=tree-or-use-legality",
              b);
          slp_journal_rollback(&journal, &work);
        }
      }
      if (!transformed && !slp_reduce_quad_disabled()) {
        for (size_t i = end; i-- > start && !transformed;) {
          if (work.data[i].op != NYIR_ADD_I64)
            continue;
          for (int swap = 0; swap < 2 && !transformed; ++swap) {
            slp_journal_begin(&journal, &work);
            bool red_ok = slp_try_reduce_quad_i64(&work, i, start, defs, &uses,
                                                  swap != 0);
            if (journal.failed) {
              slp_journal = NULL;
              free(journal.items);
              nyr_use_def_free(&uses);
              free(defs);
              nyr_cfg_free(&cfg);
              nyr_ir_func_free(&work);
              return false;
            }
            if (red_ok) {
              char err[256] = {0};
              if (nyr_ir_verify(&work, err, sizeof(err))) {
                ny_dbg_heuristic_event("SLP",
                                       "decision=accept kind=reduce_quad block=%zu "
                                       "lanes=4 reason=pack-legal-and-verifies",
                                       b);
                if (!slp_accept(&journal, &work)) {
                  free(journal.items);
                  nyr_use_def_free(&uses);
                  free(defs);
                  nyr_cfg_free(&cfg);
                  return false;
                }
                transformed = true;
                changed = true;
                break;
              }
              ny_dbg_heuristic_event("SLP",
                                     "decision=reject kind=reduce_quad block=%zu "
                                     "reason=verification detail=%s",
                                     b, err[0] ? err : "unknown");
            }
            slp_journal_rollback(&journal, &work);
          }
        }
      }
    }
    nyr_use_def_free(&uses);
    free(defs);
    nyr_cfg_free(&cfg);
    if (!transformed)
      break;
  }

  free(journal.items);
  if (!changed) {
    nyr_ir_func_free(&work);
    return true;
  }
  nyr_func_replace(f, &work);
  return true;
}
