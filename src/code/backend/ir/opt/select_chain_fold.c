/*
 * Fold compare/select chains that share one controlling condition.
 *
 * If-conversion turns `if x == K { x = x + a } else { x = x + b }` into
 *
 *   c  = eq x, K
 *   x' = select c, x + a, x + b
 *
 * A following step compares x' against another constant. Evaluating that
 * compare on each select arm (the true arm knows x == K, the false arm has
 * whatever range x carries) often proves that it is decided by `c` itself.
 * Then the next select is controlled by `c` too, and its arms can read the
 * corresponding arm of x' directly. Repeating this collapses a chain of N
 * diamonds into one select of two closed-form values.
 *
 * Rules (all exact for wrapping i64):
 *   R1  select(eq(v, K), add(v, C), F) -> select(.., K + C, F)
 *       when the add has no other use.
 *   R2  eq(select(c, A, B), K2) -> c        when A == K2 and B != K2
 *       eq(select(c, A, B), K2) -> const r  when both arms decide to r
 *       An arm base + off equals K2 iff base == K2 - off; `base != K2 - off`
 *       is proved by the base's range.
 *   R3  select(c, add(x, C1), add(x, C2)) with x = select(c, A, B)
 *       -> select(c, add(A, C1), add(B, C2)), each add having no other use.
 *   R4  add(add(b, C1), C2) -> add(b, C1 + C2) when C2 has no other use.
 */
#include "code/backend/ir/opt/loop.h"
#include "code/backend/ir/opt/util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  nyr_func_t *f;
  int *defs;
  size_t *uses;
  nyr_value_fact_t *facts;
  nyr_scev_info_t scev;
  bool scev_ok;
  nyr_cfg_t cfg;
  bool cfg_ok;
} scf_t;

static const nyr_inst_t *scf_def(const scf_t *s, int v) {
  if (v < 0 || v >= s->f->next_value || s->defs[v] < 0)
    return NULL;
  return &s->f->data[s->defs[v]];
}

static int scf_strip(const scf_t *s, int v) {
  for (int depth = 0; depth < 16; ++depth) {
    const nyr_inst_t *d = scf_def(s, v);
    if (!d || d->op != NYIR_COPY || d->a < 0)
      return v;
    v = d->a;
  }
  return v;
}

static bool scf_const(const scf_t *s, int v, int64_t *out) {
  const nyr_inst_t *d = scf_def(s, scf_strip(s, v));
  if (!d || d->op != NYIR_CONST_I64)
    return false;
  *out = d->imm;
  return true;
}

/*
 * v = base + off through a chain of add-constant instructions.
 */
static int scf_base_offset(const scf_t *s, int v, int64_t *off) {
  *off = 0;
  v = scf_strip(s, v);
  for (int depth = 0; depth < 4096; ++depth) {
    const nyr_inst_t *d = scf_def(s, v);
    int64_t c = 0;
    if (!d || d->op != NYIR_ADD_I64)
      return v;
    if (scf_const(s, d->b, &c)) {
      *off = (int64_t)((uint64_t)*off + (uint64_t)c);
      v = scf_strip(s, d->a);
    } else if (scf_const(s, d->a, &c)) {
      *off = (int64_t)((uint64_t)*off + (uint64_t)c);
      v = scf_strip(s, d->b);
    } else {
      return v;
    }
  }
  return v;
}

/*
 * Lower bound of a counted induction PHI: iv = phi(init, iv + 1) whose
 * header leaves the loop when `iv < limit` is false. Every increment then
 * runs with iv < limit <= INT64_MAX, so it cannot wrap and iv >= init.
 */
static bool scf_iv_min(scf_t *s, int v, int64_t *min) {
  if (!s->cfg_ok)
    return false;
  const nyr_inst_t *phi = scf_def(s, v);
  if (!phi || phi->op != NYIR_PHI || phi->phi_incoming_len != 2)
    return false;
  const nyr_cfg_t *cfg = &s->cfg;
  size_t h = cfg->inst_block[s->defs[v]];
  int init = -1, back = -1;
  size_t latch = SIZE_MAX;
  for (size_t k = 0; k < 2; ++k) {
    size_t p = 0;
    if (!nyr_cfg_block_for_label(cfg, phi->phi_incoming[k].predecessor_label,
                                 &p))
      return false;
    if (nyr_cfg_is_backedge(cfg, p, h)) {
      back = phi->phi_incoming[k].value;
      latch = p;
    } else {
      init = phi->phi_incoming[k].value;
    }
  }
  int64_t c0 = 0, step = 0;
  if (init < 0 || back < 0 || !scf_const(s, init, &c0))
    return false;
  const nyr_inst_t *inc = scf_def(s, scf_strip(s, back));
  if (!inc || inc->op != NYIR_ADD_I64)
    return false;
  if (!((scf_strip(s, inc->a) == v && scf_const(s, inc->b, &step)) ||
        (scf_strip(s, inc->b) == v && scf_const(s, inc->a, &step))) ||
      step != 1)
    return false;
  /*
   * Header guard: br.if (iv < limit) -> body, with the other edge exiting.
   */
  size_t end = nyr_block_last_non_nop(s->f, cfg, h);
  if (end <= cfg->block_start[h])
    return false;
  const nyr_inst_t *br = &s->f->data[end - 1];
  if (br->op != NYIR_BR_IF)
    return false;
  const nyr_inst_t *cmp = scf_def(s, scf_strip(s, br->a));
  if (!cmp || cmp->op != NYIR_CMP_I64 || cmp->cmp != NYIR_CMP_LT ||
      scf_strip(s, cmp->a) != v)
    return false;
  size_t body = 0;
  if (!nyr_cfg_block_for_label(cfg, br->imm, &body))
    return false;
  bool *member = calloc(cfg->block_count, sizeof(*member));
  if (!member)
    return false;
  bool ok =
      nyr_cfg_natural_loop_blocks(cfg, latch, h, member, cfg->block_count) &&
      member[body];
  for (size_t k = cfg->succ_offsets[h]; ok && k < cfg->succ_offsets[h + 1];
       ++k) {
    size_t succ = cfg->succ_blocks[k];
    if (succ != body && member[succ])
      ok = false;
  }
  free(member);
  if (ok)
    *min = c0;
  return ok;
}

/*
 * Intersect a bound from one source into the running range.
 */
static void scf_meet(nyr_range_t *r, const nyr_range_t *add) {
  if (add->has_min && (!r->has_min || add->min > r->min)) {
    r->has_min = true;
    r->min = add->min;
  }
  if (add->has_max && (!r->has_max || add->max < r->max)) {
    r->has_max = true;
    r->max = add->max;
  }
}

/*
 * Combine value facts, SCEV, and the induction lower bound; each source is
 * sound on its own, so their intersection is too.
 */
static bool scf_range(scf_t *s, int v, size_t at, nyr_range_t *r) {
  *r = (nyr_range_t){0};
  nyr_range_t part = {0};
  if (s->facts && nir_value_range_at(s->f, s->facts, v, at, &part))
    scf_meet(r, &part);
  if (s->scev_ok && s->defs && !(r->has_min && r->has_max)) {
    nyr_scev_affine_t aff;
    if (nyr_scev_affine_at(s->f, &s->scev, s->defs, v, at, &aff))
      scf_meet(r, &aff.range);
  }
  int64_t min = 0;
  if (!r->has_min && scf_iv_min(s, v, &min))
    scf_meet(r, &(nyr_range_t){.has_min = true, .min = min});
  return r->has_min || r->has_max;
}

/*
 * 1 = arm equals k, 0 = arm differs from k, -1 = unknown.
 */
static int scf_arm_eq(scf_t *s, int arm, int64_t k, size_t at) {
  int64_t c = 0;
  if (scf_const(s, arm, &c))
    return c == k;
  int64_t off = 0;
  int base = scf_base_offset(s, arm, &off);
  int64_t target = (int64_t)((uint64_t)k - (uint64_t)off);
  int64_t bc = 0;
  if (scf_const(s, base, &bc))
    return bc == target;
  nyr_range_t r;
  if (!scf_range(s, base, at, &r))
    return -1;
  if ((r.has_min && target < r.min) || (r.has_max && target > r.max))
    return 0;
  return -1;
}

static bool scf_is_eq_const(const scf_t *s, int cond, int *v, int64_t *k) {
  const nyr_inst_t *d = scf_def(s, scf_strip(s, cond));
  if (!d || d->op != NYIR_CMP_I64 || d->cmp != NYIR_CMP_EQ)
    return false;
  if (scf_const(s, d->b, k)) {
    *v = scf_strip(s, d->a);
    return true;
  }
  if (scf_const(s, d->a, k)) {
    *v = scf_strip(s, d->b);
    return true;
  }
  return false;
}

static bool scf_init(scf_t *s, nyr_func_t *f) {
  memset(s, 0, sizeof(*s));
  s->f = f;
  if (f->next_value <= 0)
    return false;
  size_t n = (size_t)f->next_value;
  s->defs = nyr_build_defs(f);
  s->uses = calloc(n, sizeof(*s->uses));
  s->facts = calloc(n, sizeof(*s->facts));
  if (!s->defs || !s->uses || !s->facts)
    return false;
  if (!nyr_analyze_values(f, s->facts, n, NULL, 0)) {
    free(s->facts);
    s->facts = NULL;
  }
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->op == NYIR_NOP)
      continue;
    int ops[6] = {in->a, in->b, in->c, in->d, in->e, in->f};
    for (int k = 0; k < 6; ++k)
      if (ops[k] >= 0 && ops[k] < f->next_value)
        s->uses[ops[k]]++;
    for (size_t k = 0; in->extra_args && k < in->extra_args_len; ++k)
      if (in->extra_args[k] >= 0 && in->extra_args[k] < f->next_value)
        s->uses[in->extra_args[k]]++;
    for (size_t k = 0; in->phi_incoming && k < in->phi_incoming_len; ++k) {
      int v = in->phi_incoming[k].value;
      if (v >= 0 && v < f->next_value)
        s->uses[v]++;
    }
  }
  /*
   * Earlier rounds leave folded selects/compares dead until DCE runs. Do not
   * count their reads: a reverse walk sees every user of a value before its
   * definition (PHIs, the only backward readers, are never pruned).
   */
  for (size_t i = f->len; i-- > 0;) {
    const nyr_inst_t *in = &f->data[i];
    if (in->dst < 0 || in->dst >= f->next_value || s->uses[in->dst])
      continue;
    int ops[3] = {-1, -1, -1};
    switch (in->op) {
    case NYIR_SELECT_I64:
      ops[2] = in->c;
      /* fall through */
    case NYIR_ADD_I64:
    case NYIR_CMP_I64:
      ops[1] = in->b;
      /* fall through */
    case NYIR_COPY:
      ops[0] = in->a;
      break;
    default:
      continue;
    }
    for (int k = 0; k < 3; ++k)
      if (ops[k] >= 0 && ops[k] < f->next_value && s->uses[ops[k]])
        s->uses[ops[k]]--;
  }
  s->scev_ok =nyr_has_loop(f) && nyr_scev_analyze(f, &s->scev);
  s->cfg_ok = nyr_has_loop(f) && nyr_cfg_build(f, &s->cfg);
  return true;
}

static void scf_free(scf_t *s) {
  free(s->defs);
  free(s->uses);
  free(s->facts);
  if (s->scev_ok)
    nyr_scev_free(&s->scev);
  if (s->cfg_ok)
    nyr_cfg_free(&s->cfg);
  memset(s, 0, sizeof(*s));
}

static bool scf_single_add_use(const scf_t *s, int v) {
  const nyr_inst_t *d = scf_def(s, v);
  return d && d->op == NYIR_ADD_I64 && s->uses[v] == 1;
}

/*
 * Keep use counts exact while rules rewrite operands: dropping the last use
 * of a pure value releases its own operands. Folded selects and compares die
 * this way, so the adds they fed become single-use within the same round.
 */
static void scf_drop_use(scf_t *s, int v) {
  for (int depth = 0; v >= 0 && v < s->f->next_value && depth < 8192;
       ++depth) {
    if (s->uses[v] == 0 || --s->uses[v] != 0)
      return;
    const nyr_inst_t *d = scf_def(s, v);
    if (!d)
      return;
    switch (d->op) {
    case NYIR_SELECT_I64:
      scf_drop_use(s, d->b);
      scf_drop_use(s, d->c);
      v = d->a;
      break;
    case NYIR_ADD_I64:
    case NYIR_CMP_I64:
      scf_drop_use(s, d->b);
      v = d->a;
      break;
    case NYIR_COPY:
      v = d->a;
      break;
    default:
      return;
    }
  }
}

static void scf_add_use(scf_t *s, int v) {
  if (v >= 0 && v < s->f->next_value)
    s->uses[v]++;
}

/*
 * Replace an add's x operand (x = the select being bypassed).
 */
static bool scf_retarget_add(scf_t *s, int add, int x, int arm) {
  nyr_inst_t *d = &s->f->data[s->defs[add]];
  int64_t c = 0;
  int old = -1;
  if (scf_strip(s, d->a) == x && scf_const(s, d->b, &c)) {
    old = d->a;
    d->a = arm;
  } else if (scf_strip(s, d->b) == x && scf_const(s, d->a, &c)) {
    old = d->b;
    d->b = arm;
  } else {
    return false;
  }
  scf_add_use(s, arm);
  scf_drop_use(s, old);
  int64_t ac = 0;
  if (scf_const(s, arm, &ac)) {
    int a = d->a, b = d->b;
    nir_make_const(d, (int64_t)((uint64_t)ac + (uint64_t)c));
    scf_drop_use(s, a);
    scf_drop_use(s, b);
  }
  return true;
}

/*
 * R4: add(add(b, C1), C2) -> add(b, C1 + C2). b dominates the inner add,
 * which dominates this one, so the rewrite stays in SSA form. The sum needs a
 * constant slot: C2's own constant when nothing else uses it, otherwise the
 * inner add itself when this add is its only user (it becomes the constant
 * and this add reads b directly). R3 builds such chains, one link per folded
 * diamond; without this a 500-step chain stays a 500-instruction dependency
 * chain.
 */
static bool scf_merge_add_const(scf_t *s, size_t at) {
  nyr_inst_t *d = &s->f->data[at];
  for (int side = 0; side < 2; ++side) {
    int cv = side ? d->a : d->b;
    int inner_raw = side ? d->b : d->a;
    int inner = scf_strip(s, inner_raw);
    int64_t c2 = 0;
    if (!scf_const(s, cv, &c2))
      continue;
    const nyr_inst_t *id = scf_def(s, inner);
    if (!id || id->op != NYIR_ADD_I64)
      continue;
    int64_t c1 = 0;
    int base = -1;
    if (scf_const(s, id->b, &c1))
      base = id->a;
    else if (scf_const(s, id->a, &c1))
      base = id->b;
    if (base < 0)
      return false;
    int64_t sum = (int64_t)((uint64_t)c1 + (uint64_t)c2);
    const nyr_inst_t *cd = scf_def(s, cv);
    if (cd && cd->op == NYIR_CONST_I64 && s->uses[cv] == 1) {
      nir_make_const(&s->f->data[s->defs[cv]], sum);
      scf_add_use(s, base);
      if (side)
        d->b = base;
      else
        d->a = base;
      scf_drop_use(s, inner_raw);
      return true;
    }
    if (inner_raw == inner && s->uses[inner] == 1) {
      nyr_inst_t *iw = &s->f->data[s->defs[inner]];
      int ia = iw->a, ib = iw->b;
      /* base keeps its use: it moves from the inner add to this one. */
      scf_add_use(s, base);
      nir_make_const(iw, sum);
      scf_drop_use(s, ia);
      scf_drop_use(s, ib);
      if (side) {
        d->b = base;
        d->a = inner;
      } else {
        d->a = base;
        d->b = inner;
      }
      scf_drop_use(s, cv);
      return true;
    }
    return false;
  }
  return false;
}

static bool scf_round(nyr_func_t *f, bool *changed) {
  scf_t s;
  if (!scf_init(&s, f)) {
    scf_free(&s);
    return true;
  }
  for (size_t i = 0; i < f->len; ++i) {
    nyr_inst_t *in = &f->data[i];
    if (in->op == NYIR_SELECT_I64) {
      int v = -1;
      int64_t k = 0;
      /*
       * R1
       */
      if (scf_is_eq_const(&s, in->a, &v, &k) && scf_single_add_use(&s, in->b)) {
        nyr_inst_t *t = &f->data[s.defs[in->b]];
        int64_t c = 0;
        if ((scf_strip(&s, t->a) == v && scf_const(&s, t->b, &c)) ||
            (scf_strip(&s, t->b) == v && scf_const(&s, t->a, &c))) {
          int ta = t->a, tb = t->b;
          nir_make_const(t, (int64_t)((uint64_t)k + (uint64_t)c));
          scf_drop_use(&s, ta);
          scf_drop_use(&s, tb);
          *changed = true;
        }
      }
      /*
       * R3: each arm independently reads the matching arm of x.
       */
      int cond = scf_strip(&s, in->a);
      for (int arm = 0; arm < 2; ++arm) {
        int av = arm ? in->c : in->b;
        if (!scf_single_add_use(&s, av))
          continue;
        const nyr_inst_t *p = &f->data[s.defs[av]];
        for (int side = 0; side < 2; ++side) {
          int x = scf_strip(&s, side ? p->b : p->a);
          const nyr_inst_t *xd = scf_def(&s, x);
          if (!xd || xd->op != NYIR_SELECT_I64 || scf_strip(&s, xd->a) != cond)
            continue;
          if (scf_retarget_add(&s, av, x, arm ? xd->c : xd->b))
            *changed = true;
          break;
        }
      }
      continue;
    }
    /*
     * R4
     */
    if (in->op == NYIR_ADD_I64 && in->dst >= 0 && scf_merge_add_const(&s, i))
      *changed = true;
    /*
     * R2
     */
    if (in->op == NYIR_CMP_I64 && in->cmp == NYIR_CMP_EQ && in->dst >= 0) {
      int sel = -1;
      int64_t k2 = 0;
      if (scf_const(&s, in->b, &k2))
        sel = scf_strip(&s, in->a);
      else if (scf_const(&s, in->a, &k2))
        sel = scf_strip(&s, in->b);
      const nyr_inst_t *sd = sel >= 0 ? scf_def(&s, sel) : NULL;
      if (!sd || sd->op != NYIR_SELECT_I64)
        continue;
      int ra = scf_arm_eq(&s, sd->b, k2, i);
      int rb = scf_arm_eq(&s, sd->c, k2, i);
      if (ra < 0 || rb < 0)
        continue;
      int ia = in->a, ib = in->b;
      if (ra == rb) {
        nir_make_const(in, ra);
        scf_drop_use(&s, ia);
        scf_drop_use(&s, ib);
        *changed = true;
      } else if (ra == 1) {
        /*
         * A boolean compare result is 0/1, like the select condition.
         */
        const nyr_inst_t *cd = scf_def(&s, scf_strip(&s, sd->a));
        if (cd && cd->op == NYIR_CMP_I64) {
          int cond = scf_strip(&s, sd->a);
          scf_add_use(&s, cond);
          nir_make_copy(in, cond);
          scf_drop_use(&s, ia);
          scf_drop_use(&s, ib);
          *changed = true;
        }
      }
    }
  }
  scf_free(&s);
  return true;
}

bool nyr_select_chain_fold(nyr_func_t *f) {
  if (!f || !f->len || getenv("NYTRIX_NO_SELECT_CHAIN_FOLD"))
    return true;
  /*
   * Each round folds every link whose inputs are already in closed form, in
   * program order, so one round normally finishes a forward chain. Bound the
   * rounds anyway; every rule strictly reduces select dependencies.
   */
  for (int round = 0; round < 4; ++round) {
    bool changed = false;
    if (!scf_round(f, &changed))
      return false;
    if (!changed)
      break;
  }
  return true;
}

/*
 * Fold float zero tests whose operand is provably nonzero.
 *
 * `1.0 / float(n)` lowers to a guard `cmp.f64 eq d, 0.0` that branches to a
 * panic. When `d` is built from integers with known ranges, the guard is
 * dead; folding it removes the panic call from hot loops, which also lets
 * loop register homes apply. Intervals are evaluated exactly: integer steps
 * reject any possible overflow, and float steps rely on IEEE rounding being
 * monotone, so a rounded bound never crosses zero when the true bound does
 * not. NaN never arises from integer conversions and finite constants.
 */
typedef struct {
  int64_t lo, hi;
} scf_ival_t;

static bool scf_int_interval(scf_t *s, int v, size_t at, int depth,
                             scf_ival_t *out) {
  if (depth > 24)
    return false;
  v = scf_strip(s, v);
  const nyr_inst_t *d = scf_def(s, v);
  if (!d)
    return false;
  scf_ival_t a, b;
  int64_t k = 0;
  switch (d->op) {
  case NYIR_CONST_I64:
    *out = (scf_ival_t){d->imm, d->imm};
    return true;
  case NYIR_ADD_I64:
    if (!scf_int_interval(s, d->a, at, depth + 1, &a) ||
        !scf_int_interval(s, d->b, at, depth + 1, &b) ||
        __builtin_add_overflow(a.lo, b.lo, &out->lo) ||
        __builtin_add_overflow(a.hi, b.hi, &out->hi))
      return false;
    return true;
  case NYIR_MUL_I64: {
    if (!scf_int_interval(s, d->a, at, depth + 1, &a) ||
        !scf_int_interval(s, d->b, at, depth + 1, &b))
      return false;
    int64_t p[4];
    if (__builtin_mul_overflow(a.lo, b.lo, &p[0]) ||
        __builtin_mul_overflow(a.lo, b.hi, &p[1]) ||
        __builtin_mul_overflow(a.hi, b.lo, &p[2]) ||
        __builtin_mul_overflow(a.hi, b.hi, &p[3]))
      return false;
    out->lo = out->hi = p[0];
    for (int i = 1; i < 4; ++i) {
      out->lo = p[i] < out->lo ? p[i] : out->lo;
      out->hi = p[i] > out->hi ? p[i] : out->hi;
    }
    return true;
  }
  case NYIR_SAR_I64:
    if (!scf_const(s, d->b, &k) || k < 0 || k > 63 ||
        !scf_int_interval(s, d->a, at, depth + 1, &a))
      return false;
    *out = (scf_ival_t){a.lo >> k, a.hi >> k};
    return true;
  default:
    break;
  }
  nyr_range_t r;
  if (scf_range(s, v, at, &r) && r.has_min && r.has_max) {
    *out = (scf_ival_t){r.min, r.max};
    return true;
  }
  return false;
}

typedef struct {
  double lo, hi;
} scf_fval_t;

static double scf_bits_f64(int64_t bits) {
  double d;
  memcpy(&d, &bits, sizeof(d));
  return d;
}

static bool scf_f64_interval(scf_t *s, int v, size_t at, int depth,
                             scf_fval_t *out) {
  if (depth > 24)
    return false;
  v = scf_strip(s, v);
  const nyr_inst_t *d = scf_def(s, v);
  if (!d)
    return false;
  scf_fval_t a, b;
  switch (d->op) {
  case NYIR_CONST_F64: {
    double x = scf_bits_f64(d->imm);
    if (x != x)
      return false;
    *out = (scf_fval_t){x, x};
    return true;
  }
  case NYIR_I64_TO_F64: {
    scf_ival_t iv;
    if (!scf_int_interval(s, d->a, at, depth + 1, &iv))
      return false;
    *out = (scf_fval_t){(double)iv.lo, (double)iv.hi};
    return true;
  }
  case NYIR_ADD_F64:
    if (!scf_f64_interval(s, d->a, at, depth + 1, &a) ||
        !scf_f64_interval(s, d->b, at, depth + 1, &b))
      return false;
    *out = (scf_fval_t){a.lo + b.lo, a.hi + b.hi};
    return out->lo == out->lo && out->hi == out->hi;
  case NYIR_MUL_F64:
    if (!scf_f64_interval(s, d->a, at, depth + 1, &a) ||
        !scf_f64_interval(s, d->b, at, depth + 1, &b) || a.lo < 0 || b.lo < 0)
      return false;
    *out = (scf_fval_t){a.lo * b.lo, a.hi * b.hi};
    return out->lo == out->lo && out->hi == out->hi;
  default:
    return false;
  }
}

static bool scf_is_f64_zero(const scf_t *s, int v) {
  const nyr_inst_t *d = scf_def(s, scf_strip(s, v));
  return d && (d->op == NYIR_CONST_F64 || d->op == NYIR_CONST_I64) &&
         (d->imm == 0 || (uint64_t)d->imm == UINT64_C(0x8000000000000000));
}

bool nyr_float_zero_guard_fold(nyr_func_t *f) {
  if (!f || !f->len || getenv("NYTRIX_NO_FLOAT_GUARD_FOLD"))
    return true;
  bool any = false;
  for (size_t i = 0; i < f->len && !any; ++i)
    any = ((f->data[i].op == NYIR_CMP_F64 || f->data[i].op == NYIR_CMP_I64) &&
           (f->data[i].cmp == NYIR_CMP_EQ || f->data[i].cmp == NYIR_CMP_NE)) ||
          f->data[i].op == NYIR_I64_TO_F64;
  if (!any)
    return true;
  scf_t s;
  if (!scf_init(&s, f)) {
    scf_free(&s);
    return true;
  }
  /*
   * Fold i64->f64 of constants first so guards see a real 0.0.
   */
  for (size_t i = 0; i < f->len; ++i) {
    nyr_inst_t *in = &f->data[i];
    int64_t c = 0;
    if (in->op != NYIR_I64_TO_F64 || in->dst < 0 || !scf_const(&s, in->a, &c))
      continue;
    double d = (double)c;
    int64_t bits = 0;
    memcpy(&bits, &d, sizeof(bits));
    nir_make_f64_const(in, bits);
  }
  for (size_t i = 0; i < f->len; ++i) {
    nyr_inst_t *in = &f->data[i];
    /*
     * The source guard may compare the raw f64 word against integer zero.
     * That is still exact here: the interval below only succeeds for a
     * value produced by float operations, and a nonzero float never has an
     * all-zero (or sign-only) bit pattern.
     */
    if ((in->op != NYIR_CMP_F64 && in->op != NYIR_CMP_I64) || in->dst < 0 ||
        (in->cmp != NYIR_CMP_EQ && in->cmp != NYIR_CMP_NE))
      continue;
    int other = scf_is_f64_zero(&s, in->b)   ? in->a
                : scf_is_f64_zero(&s, in->a) ? in->b
                                             : -1;
    scf_fval_t r;
    if (other < 0 || !scf_f64_interval(&s, other, i, 0, &r))
      continue;
    if (r.lo > 0.0 || r.hi < 0.0)
      nir_make_const(in, in->cmp == NYIR_CMP_NE);
  }
  scf_free(&s);
  return true;
}
