/*
 * Critical-path reassociation for innermost loop recurrences.
 *
 * Earlier scalar passes minimise instruction count, which can put an
 * operation on a loop-carried dependency chain that did not need to be
 * there. Two common shapes are:
 *
 *   select(c, a, b) + k   -> select(c, a + k, b + k)
 *   (late +/- early) +/- r -> (early +/- r) +/- late
 *
 * The first is the inverse of peephole's shared-addend factoring and costs
 * one extra ADD; the second is free. Both are exact for wrapping i64 ADD.
 *
 * Each innermost natural loop is simulated for a few iterations with a
 * small latency model, so loop-carried PHIs take the ready time of their
 * backedge value and loop-invariant inputs stay at zero. A rewrite is kept
 * only when it strictly shortens the modelled ready time of its result.
 * Every rewrite rebuilds the analysis, and the total number of rewrites per
 * function is bounded.
 */
#include "code/backend/passes/util.h"
#include <stdlib.h>
#include <string.h>

enum {
  ILP_ROUNDS = 4,
  ILP_MAX_REWRITES = 64,
  ILP_MAX_LOOP_INSTS = 4096,
};

typedef struct {
  nyr_cfg_t cfg;
  int *defs;
  int *uses;
  int *time;
  bool *member;
  bool cfg_ok;
} ilp_state_t;

static void ilp_state_free(ilp_state_t *s) {
  if (s->cfg_ok)
    nyr_cfg_free(&s->cfg);
  free(s->defs);
  free(s->uses);
  free(s->time);
  free(s->member);
  memset(s, 0, sizeof(*s));
}

static void ilp_count_use(int *uses, int limit, int value) {
  if (value >= 0 && value < limit)
    uses[value]++;
}

static bool ilp_state_init(ilp_state_t *s, const nyr_func_t *f) {
  memset(s, 0, sizeof(*s));
  if (f->next_value <= 0)
    return false;
  size_t values = (size_t)f->next_value;
  s->defs = nyr_build_defs(f);
  s->uses = calloc(values, sizeof(*s->uses));
  s->time = calloc(values, sizeof(*s->time));
  if (!s->defs || !s->uses || !s->time || !nyr_cfg_build(f, &s->cfg))
    return false;
  s->cfg_ok = true;
  s->member = calloc(s->cfg.block_count ? s->cfg.block_count : 1,
                     sizeof(*s->member));
  if (!s->member)
    return false;
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    const int ops[] = {in->a, in->b, in->c, in->d, in->e, in->f};
    for (size_t k = 0; k < sizeof(ops) / sizeof(ops[0]); ++k)
      ilp_count_use(s->uses, f->next_value, ops[k]);
    for (size_t k = 0; k < in->extra_args_len; ++k)
      ilp_count_use(s->uses, f->next_value, in->extra_args[k]);
    for (size_t k = 0; k < in->phi_incoming_len; ++k)
      ilp_count_use(s->uses, f->next_value, in->phi_incoming[k].value);
  }
  return true;
}

static bool ilp_const(const nyr_func_t *f, const int *defs, int value,
                      int64_t *out) {
  if (value < 0 || value >= f->next_value || defs[value] < 0)
    return false;
  const nyr_inst_t *def = &f->data[defs[value]];
  if (def->op != NYIR_CONST_I64)
    return false;
  *out = def->imm;
  return true;
}

/*
 * Approximate x86-64/AArch64 result latency in cycles.
 */
static int ilp_latency(const nyr_func_t *f, const int *defs,
                       const nyr_inst_t *in) {
  int64_t k = 0;
  switch (in->op) {
  case NYIR_NOP:
  case NYIR_LABEL:
  case NYIR_CONST_I64:
  case NYIR_COPY:
    return 0;
  case NYIR_MUL_I64:
    /*
     * Small multipliers lower to one shift or scaled LEA (2 cycles on
     * Zen and on the slow-LEA Intel cores).
     */
    if ((ilp_const(f, defs, in->b, &k) || ilp_const(f, defs, in->a, &k)) &&
        (k == 2 || k == 4 || k == 8))
      return 1;
    if ((ilp_const(f, defs, in->b, &k) || ilp_const(f, defs, in->a, &k)) &&
        (k == 3 || k == 5 || k == 9))
      return 2;
    return 3;
  case NYIR_DIV_I64:
  case NYIR_MOD_I64:
    if (ilp_const(f, defs, in->b, &k) && k > 1 && (k & (k - 1)) == 0)
      return 3;
    return 25;
  case NYIR_LOAD_I64:
  case NYIR_LOAD_LOCAL:
    return 4;
  case NYIR_CALL:
    return 20;
  default:
    return 1;
  }
}

static int ilp_time(const ilp_state_t *s, const nyr_func_t *f, int value) {
  return value >= 0 && value < f->next_value ? s->time[value] : 0;
}

/*
 * Simulate ILP_ROUNDS iterations of the loop whose blocks are marked in
 * s->member. Header PHIs take the ready time their latch value had in the
 * previous round; everything defined outside the loop stays at zero.
 */
static void ilp_simulate(ilp_state_t *s, const nyr_func_t *f, size_t header,
                         int64_t latch_label) {
  const nyr_cfg_t *cfg = &s->cfg;
  for (int round = 0; round < ILP_ROUNDS; ++round) {
    for (size_t block = 0; block < cfg->block_count; ++block) {
      if (!s->member[block])
        continue;
      for (size_t i = cfg->block_start[block]; i < cfg->block_end[block];
           ++i) {
        const nyr_inst_t *in = &f->data[i];
        if (in->dst < 0 || in->dst >= f->next_value)
          continue;
        if (in->op == NYIR_PHI) {
          int t = 0;
          if (block == header && round > 0)
            for (size_t k = 0; k < in->phi_incoming_len; ++k)
              if (in->phi_incoming[k].predecessor_label == latch_label)
                t = ilp_time(s, f, in->phi_incoming[k].value);
          s->time[in->dst] = t;
          continue;
        }
        int t = 0;
        const int ops[] = {in->a, in->b, in->c, in->d, in->e, in->f};
        for (size_t k = 0; k < sizeof(ops) / sizeof(ops[0]); ++k) {
          int u = ilp_time(s, f, ops[k]);
          if (u > t)
            t = u;
        }
        s->time[in->dst] = t + ilp_latency(f, s->defs, in);
      }
    }
  }
}

/*
 * A pure, unflagged wrapping i64 ADD/SUB whose operands are both values.
 */
static bool ilp_plain_addsub(const nyr_inst_t *in) {
  return (in->op == NYIR_ADD_I64 || in->op == NYIR_SUB_I64) && in->dst >= 0 && in->a >= 0 && in->b >= 0 &&
         in->flags == 0 && in->extra_args_len == 0 &&
         nyr_effective_effects(in) == NYIR_EFFECT_NONE &&
         (in->scalar_class == NYIR_SCALAR_I64_RAW ||
          in->scalar_class == NYIR_SCALAR_UNKNOWN);
}

static const nyr_inst_t *ilp_def(const ilp_state_t *s, const nyr_func_t *f,
                                 int value, size_t *index) {
  if (value < 0 || value >= f->next_value || s->defs[value] < 0)
    return NULL;
  *index = (size_t)s->defs[value];
  return &f->data[*index];
}

static int ilp_max(int a, int b) { return a > b ? a : b; }

static nyr_inst_t ilp_make_add(int dst, int a, int b,
                               nyr_scalar_class_t scalar_class,
                               nyr_debug_loc_t debug) {
  return (nyr_inst_t){.op = NYIR_ADD_I64,
                      .dst = dst,
                      .a = a,
                      .b = b,
                      .c = -1,
                      .d = -1,
                      .e = -1,
                      .f = -1,
                      .effects = NYIR_EFFECT_NONE,
                      .scalar_class = scalar_class,
                      .debug = debug};
}

/*
 * select(c, a, b) + k -> select(c, a + k, b + k).
 */
static int ilp_try_select_add(ilp_state_t *s, nyr_func_t *f, size_t at) {
  const nyr_inst_t *w = &f->data[at];
  if (w->op != NYIR_ADD_I64 || !ilp_plain_addsub(w))
    return 0;
  size_t block = s->cfg.inst_block[at];
  for (int side = 0; side < 2; ++side) {
    int sel_value = side ? w->b : w->a;
    int k = side ? w->a : w->b;
    size_t sel_at = 0;
    const nyr_inst_t *sel = ilp_def(s, f, sel_value, &sel_at);
    if (!sel || sel->op != NYIR_SELECT_I64 || sel_at >= at ||
        s->cfg.inst_block[sel_at] != block || s->uses[sel_value] != 1 ||
        sel->flags != 0 || sel->a < 0 || sel->b < 0 || sel->c < 0 ||
        nyr_effective_effects(sel) != NYIR_EFFECT_NONE ||
        (sel->scalar_class != NYIR_SCALAR_I64_RAW &&
         sel->scalar_class != NYIR_SCALAR_UNKNOWN))
      continue;
    int tc = ilp_time(s, f, sel->a), ta = ilp_time(s, f, sel->b),
        tb = ilp_time(s, f, sel->c), tk = ilp_time(s, f, k);
    int old_time = ilp_max(ilp_max(ilp_max(tc, ta), tb) + 1, tk) + 1;
    int new_time =
        ilp_max(tc, ilp_max(ilp_max(ta, tk), ilp_max(tb, tk)) + 1) + 1;
    if (new_time >= old_time)
      continue;
    if (f->next_value > INT32_MAX - 2 || !nir_ensure_inst_space(f, 2))
      return -1;
    /*
     * nir_ensure_inst_space may move f->data.
     */
    w = &f->data[at];
    sel = &f->data[sel_at];
    const int yes = sel->b, no = sel->c, dst = w->dst;
    const nyr_scalar_class_t cls = w->scalar_class;
    const nyr_debug_loc_t debug = w->debug;
    nyr_inst_t new_sel = *sel;
    new_sel.dst = dst;
    nyr_clear_derived_facts(&new_sel);
    new_sel.scalar_class = w->scalar_class;
    new_sel.semantic_ownership = w->semantic_ownership;
    new_sel.semantic_mutable = w->semantic_mutable;
    const int yes_sum = f->next_value++;
    const int no_sum = f->next_value++;
    new_sel.b = yes_sum;
    new_sel.c = no_sum;
    /*
     * The SELECT's only user is w, so it can be dropped outright.
     */
    nyr_inst_discard(&f->data[sel_at]);
    memmove(&f->data[at + 2], &f->data[at], (f->len - at) * sizeof(*f->data));
    f->len += 2;
    f->data[at] = ilp_make_add(yes_sum, yes, k, cls, debug);
    f->data[at + 1] = ilp_make_add(no_sum, no, k, cls, debug);
    f->data[at + 2] = new_sel;
    return 1;
  }
  return 0;
}

/*
 * Reassociate a two-level ADD/SUB tree so its latest leaf is combined last:
 *
 *   u = t op_u r,  t = p op_t q   (t has no other user)
 *
 * Each leaf carries a sign. When p or q is the latest leaf, the two earlier
 * leaves form the new inner node and the latest joins at the root. The sign
 * algebra is exact for wrapping i64 arithmetic.
 */
static int ilp_try_add_chain(ilp_state_t *s, nyr_func_t *f, size_t at) {
  nyr_inst_t *u = &f->data[at];
  if (!ilp_plain_addsub(u))
    return 0;
  size_t block = s->cfg.inst_block[at];
  for (int side = 0; side < 2; ++side) {
    /*
     * SUB is only reassociable through its left operand.
     */
    if (side == 1 && u->op == NYIR_SUB_I64)
      break;
    int t_value = side ? u->b : u->a;
    int r = side ? u->a : u->b;
    size_t t_at = 0;
    const nyr_inst_t *t = ilp_def(s, f, t_value, &t_at);
    if (!t || !ilp_plain_addsub(t) || t_at >= at ||
        s->cfg.inst_block[t_at] != block || s->uses[t_value] != 1)
      continue;
    int leaf[3] = {t->a, t->b, r};
    int sign[3] = {1, t->op == NYIR_SUB_I64 ? -1 : 1,
                   u->op == NYIR_SUB_I64 ? -1 : 1};
    int tl[3] = {ilp_time(s, f, leaf[0]), ilp_time(s, f, leaf[1]),
                 ilp_time(s, f, leaf[2])};
    int late = tl[0] >= tl[1] ? 0 : 1;
    if (tl[late] <= tl[2])
      continue;
    int e0 = late == 0 ? 1 : 0, e1 = 2;
    int old_time = ilp_max(ilp_max(tl[0], tl[1]) + 1, tl[2]) + 1;
    int new_time = ilp_max(ilp_max(tl[e0], tl[e1]) + 1, tl[late]) + 1;
    if (new_time >= old_time)
      continue;
    /*
     * Inner node: +a+b -> add, +a-b -> sub(a,b), -a-b -> add with the
     * negation folded into the root (then the late leaf must be +).
     */
    nyr_op_t inner_op;
    int ia, ib, inner_sign;
    if (sign[e0] > 0 && sign[e1] > 0) {
      inner_op = NYIR_ADD_I64, ia = leaf[e0], ib = leaf[e1], inner_sign = 1;
    } else if (sign[e0] > 0) {
      inner_op = NYIR_SUB_I64, ia = leaf[e0], ib = leaf[e1], inner_sign = 1;
    } else if (sign[e1] > 0) {
      inner_op = NYIR_SUB_I64, ia = leaf[e1], ib = leaf[e0], inner_sign = 1;
    } else {
      inner_op = NYIR_ADD_I64, ia = leaf[e0], ib = leaf[e1], inner_sign = -1;
    }
    nyr_op_t root_op;
    int ra, rb;
    if (inner_sign > 0) {
      root_op = sign[late] > 0 ? NYIR_ADD_I64 : NYIR_SUB_I64;
      ra = t_value, rb = leaf[late];
    } else {
      if (sign[late] < 0)
        continue; /* -(a + b) - late is not two plain ops. */
      root_op = NYIR_SUB_I64, ra = leaf[late], rb = t_value;
    }
    /*
     * Rewrite t in place and u as the root. When r is defined after t,
     * sink t to just before u first; t has no other user and its remaining
     * operands are already defined, so the move is legal.
     */
    size_t r_at = 0;
    bool sink = ilp_def(s, f, r, &r_at) && r_at > t_at;
    nyr_inst_t moved = f->data[t_at];
    moved.op = inner_op;
    moved.a = ia;
    moved.b = ib;
    moved.flags = 0;
    nyr_clear_derived_facts(&moved);
    if (sink) {
      memmove(&f->data[t_at], &f->data[t_at + 1],
              (at - 1 - t_at) * sizeof(*f->data));
      f->data[at - 1] = moved;
    } else {
      f->data[t_at] = moved;
    }
    u = &f->data[at];
    u->op = root_op;
    u->a = ra;
    u->b = rb;
    nyr_clear_derived_facts(u);
    return 1;
  }
  return 0;
}

/*
 * Run one rewrite on an innermost loop. Returns 1 on change, 0 if none,
 * -1 on allocation failure.
 */
static int ilp_rewrite_once(nyr_func_t *f) {
  ilp_state_t s;
  if (!ilp_state_init(&s, f)) {
    ilp_state_free(&s);
    return 0;
  }
  const nyr_cfg_t *cfg = &s.cfg;
  int result = 0;
  for (size_t latch = 0; latch < cfg->block_count && result == 0; ++latch) {
    for (size_t e = cfg->succ_offsets[latch];
         e < cfg->succ_offsets[latch + 1] && result == 0; ++e) {
      if (!cfg->backedge_edges[e])
        continue;
      size_t header = cfg->succ_blocks[e];
      if (!nyr_cfg_natural_loop_blocks(cfg, latch, header, s.member,
                                       cfg->block_count))
        continue;
      /*
       * Innermost only: no other backedge targets a member block.
       */
      bool innermost = true;
      size_t insts = 0;
      for (size_t b = 0; b < cfg->block_count && innermost; ++b) {
        if (!s.member[b])
          continue;
        insts += cfg->block_end[b] - cfg->block_start[b];
        for (size_t x = cfg->succ_offsets[b]; x < cfg->succ_offsets[b + 1];
             ++x)
          if (cfg->backedge_edges[x] && cfg->succ_blocks[x] != header &&
              s.member[cfg->succ_blocks[x]])
            innermost = false;
      }
      if (!innermost || insts > ILP_MAX_LOOP_INSTS)
        continue;
      memset(s.time, 0, (size_t)f->next_value * sizeof(*s.time));
      ilp_simulate(&s, f, header, cfg->block_label[latch]);
      for (size_t b = 0; b < cfg->block_count && result == 0; ++b) {
        if (!s.member[b])
          continue;
        for (size_t i = cfg->block_start[b];
             i < cfg->block_end[b] && result == 0; ++i) {
          result = ilp_try_select_add(&s, f, i);
          if (result == 0)
            result = ilp_try_add_chain(&s, f, i);
        }
      }
    }
  }
  ilp_state_free(&s);
  return result;
}

bool nyr_ilp_reassociate(nyr_func_t *f) {
  if (!f || f->len == 0 || !nyr_has_loop(f))
    return true;
  for (int n = 0; n < ILP_MAX_REWRITES; ++n) {
    int r = ilp_rewrite_once(f);
    if (r < 0)
      return false;
    if (r == 0)
      break;
  }
  return true;
}
