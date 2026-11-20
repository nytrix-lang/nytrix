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
#include "base/debug.h"
#include "code/backend/passes/verify/loop.h"
#include "code/backend/passes/util.h"
#include <math.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  nyr_func_t *f;
  int *defs;
  const unsigned char *def_counts;
  size_t *uses;
  nyr_value_fact_t *facts;
  nyr_scev_info_t scev;
  bool scev_ok;
  nyr_cfg_t cfg;
  bool cfg_ok;
} scf_t;

static const nyr_inst_t *scf_def(const scf_t *s, int v) {
  if (v < 0 || v >= s->f->next_value || s->defs[v] < 0 ||
      (s->def_counts && s->def_counts[v] != 1))
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
      /*
       * fall through
       */
    case NYIR_ADD_I64:
    case NYIR_CMP_I64:
      ops[1] = in->b;
      /*
       * fall through
       */
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
  bool has_loop = nyr_has_loop(f);
  s->scev_ok = has_loop && nyr_scev_analyze(f, &s->scev);
  s->cfg_ok = has_loop && nyr_cfg_build(f, &s->cfg);
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
      /*
       * base keeps its use: it moves from the inner add to this one.
       */
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

/*
 * x*2, x + x, and x << 1 are the same value. Strength reduction picks one of
 * them per instruction, so arms that compute the same expression can still
 * differ syntactically.
 */
static bool scf_double_value(const scf_t *s, int v, int *x) {
  const nyr_inst_t *d = scf_def(s, v);
  if (!d)
    return false;
  if (d->op == NYIR_MUL_I64 || d->op == NYIR_SHL_I64) {
    int64_t want = d->op == NYIR_MUL_I64 ? 2 : 1;
    int64_t k = 0;
    if (scf_const(s, d->b, &k) && k == want) {
      *x = scf_strip(s, d->a);
      return true;
    }
    if (scf_const(s, d->a, &k) && k == want) {
      *x = scf_strip(s, d->b);
      return true;
    }
    return false;
  }
  if (d->op == NYIR_ADD_I64) {
    int a = scf_strip(s, d->a);
    if (a >= 0 && a == scf_strip(s, d->b)) {
      *x = a;
      return true;
    }
  }
  return false;
}

/*
 * Structural equality for the pure scalar expressions that if-conversion moves
 * out of diamond arms. Two arms that compute the same value from the same
 * inputs are interchangeable even when they were never CSE'd into one vreg.
 */
static bool scf_same_value(const scf_t *s, int a, int b, int depth) {
  if (a == b)
    return a >= 0;
  if (depth > 4)
    return false;
  int xa = -1, xb = -1;
  if (scf_double_value(s, a, &xa) && scf_double_value(s, b, &xb) &&
      scf_same_value(s, xa, xb, depth + 1))
    return true;
  const nyr_inst_t *da = scf_def(s, a);
  const nyr_inst_t *db = scf_def(s, b);
  if (!da || !db || da->op != db->op ||
      da->scalar_class != db->scalar_class)
    return false;
  switch (da->op) {
  case NYIR_CONST_I64:
    return da->imm == db->imm;
  case NYIR_ADD_I64:
  case NYIR_SUB_I64:
  case NYIR_MUL_I64:
  case NYIR_AND_I64:
  case NYIR_OR_I64:
  case NYIR_XOR_I64:
    return scf_same_value(s, scf_strip(s, da->a), scf_strip(s, db->a),
                          depth + 1) &&
           scf_same_value(s, scf_strip(s, da->b), scf_strip(s, db->b),
                          depth + 1);
  case NYIR_SHL_I64:
  case NYIR_SAR_I64:
  case NYIR_ROR_I64: {
    if (!scf_same_value(s, scf_strip(s, da->a), scf_strip(s, db->a),
                        depth + 1))
      return false;
    if (da->imm == db->imm && da->b == db->b)
      return true;
    int64_t ka = 0, kb = 0;
    return scf_const(s, da->b, &ka) && scf_const(s, db->b, &kb) && ka == kb;
  }
  default:
    return false;
  }
}

/*
 * A raw compare or `x & 1` is 0/1, so it is a valid arithmetic operand.
 */
static bool scf_boolean_value(const scf_t *s, int v) {
  const nyr_inst_t *d = scf_def(s, scf_strip(s, v));
  if (!d)
    return false;
  if (d->op == NYIR_CMP_I64 || d->op == NYIR_CMP_F64 ||
      d->op == NYIR_CMP_F32)
    return true;
  if (d->op == NYIR_AND_I64) {
    int64_t mask = 0;
    if (scf_const(s, d->b, &mask) && mask == 1)
      return true;
    if (scf_const(s, d->a, &mask) && mask == 1)
      return true;
  }
  return d->op == NYIR_CONST_I64 && (d->imm == 0 || d->imm == 1);
}

#define SCF_TRACE(...)                    \
  do {                                    \
    if (ny_trace_enabled("NY_TRACE_SCF")) \
      ny_trace_line("SCF", __VA_ARGS__);  \
  } while (0)

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
       * R0: a condition already folded to a constant picks one arm. Loop
       * unrolling makes every ternary in the expanded body a compile-time
       * predicate; replacing the select with a copy of the taken arm also
       * releases the dead arm so its own fold chain collapses.
       */
      int64_t cond_value = 0;
      if (scf_const(&s, in->a, &cond_value)) {
        int taken = cond_value != 0 ? in->b : in->c;
        int dead = cond_value != 0 ? in->c : in->b;
        SCF_TRACE("R0 @%zu v%d = select const %lld -> v%d", i, in->dst,
                  (long long)cond_value, taken);
        /*
         * Save the condition first: nir_make_copy overwrites `a` with the
         * taken arm, and dropping `in->a` afterwards released the taken
         * arm's use instead. Its count then reached zero while still live,
         * R4 treated shared constants as single-use and rewrote them in
         * place (a swap loop folded to 82 instead of 45).
         */
        int cond_use = in->a;
        nir_make_copy(in, taken);
        scf_drop_use(&s, cond_use);
        scf_drop_use(&s, dead);
        *changed = true;
        continue;
      }
      /*
       * R1
       */
      if (scf_is_eq_const(&s, in->a, &v, &k) && scf_single_add_use(&s, in->b)) {
        nyr_inst_t *t = &f->data[s.defs[in->b]];
        int64_t c = 0;
        if ((scf_strip(&s, t->a) == v && scf_const(&s, t->b, &c)) ||
            (scf_strip(&s, t->b) == v && scf_const(&s, t->a, &c))) {
          int ta = t->a, tb = t->b;
          SCF_TRACE("R1 @%zu v%d: v%d := %lld (eq v%d %lld)", i, in->dst,
                    in->b, (long long)(k + c), v, (long long)k);
          nir_make_const(t, (int64_t)((uint64_t)k + (uint64_t)c));
          scf_drop_use(&s, ta);
          scf_drop_use(&s, tb);
          *changed = true;
        }
      }
      /*
       * R5: select(c, t + 1, t) with a 0/1 c is t + c. The two arms are often
       * independently computed but structurally identical, so compare them by
       * expression rather than by vreg. This removes the compare and cmov that
       * a bit-by-bit radix walk would otherwise spend one mispredictable branch
       * or cmov on per key bit.
       */
      {
        int cond_v = scf_strip(&s, in->a);
        int true_v = scf_strip(&s, in->b);
        int false_v = scf_strip(&s, in->c);
        const nyr_inst_t *ad = scf_def(&s, true_v);
        if (ad && ad->op == NYIR_ADD_I64 && false_v >= 0 &&
            scf_boolean_value(&s, cond_v)) {
          for (int side = 0; side < 2; ++side) {
            int base = scf_strip(&s, side ? ad->b : ad->a);
            int64_t step = 0;
            if (!scf_const(&s, side ? ad->a : ad->b, &step) || step != 1 ||
                !scf_same_value(&s, base, false_v, 0))
              continue;
            int old_a = in->a, old_b = in->b, old_c = in->c;
            SCF_TRACE("R5 @%zu v%d = v%d + v%d", i, in->dst, false_v, cond_v);
            in->op = NYIR_ADD_I64;
            in->a = false_v;
            in->b = cond_v;
            in->c = -1;
            in->d = in->e = in->f = -1;
            in->imm = 0;
            in->scalar_class = NYIR_SCALAR_I64_RAW;
            scf_add_use(&s, false_v);
            scf_add_use(&s, cond_v);
            scf_drop_use(&s, old_a);
            scf_drop_use(&s, old_b);
            scf_drop_use(&s, old_c);
            *changed = true;
            break;
          }
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
          if (scf_retarget_add(&s, av, x, arm ? xd->c : xd->b)) {
            SCF_TRACE("R3 @%zu v%d arm %d: v%d reads v%d instead of v%d", i,
                      in->dst, arm, av, arm ? xd->c : xd->b, x);
            *changed = true;
          }
          break;
        }
      }
      continue;
    }
    /*
     * R4
     */
    if (in->op == NYIR_ADD_I64 && in->dst >= 0 && scf_merge_add_const(&s, i)) {
      SCF_TRACE("R4 @%zu v%d", i, in->dst);
      *changed = true;
    }
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
        SCF_TRACE("R2 @%zu v%d := %d", i, in->dst, ra);
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
          SCF_TRACE("R2 @%zu v%d := v%d", i, in->dst, cond);
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

/*
 * Fold a chain of recurrence steps whose equality branch advances with the
 * comparison key. With a miss increment b and hit increment a, the state
 * first hits a key exactly when x0 = k - j*(b-a), for 0 <= j < n.
 * On a hit the remaining steps stay aligned; otherwise all n steps miss.
 * A bounded range plus congruence test captures the exact hitting steps.
 * Keep keys away from signed boundaries so unrelated wrapped trajectories
 * cannot enter the bounded interval within the chain's length.
 */
static bool scf_step_add(const scf_t *s, const nyr_inst_t *add, int *prev,
                         int64_t *key, int *step, int *gap) {
  if (add->op != NYIR_ADD_I64 || add->dst < 0)
    return false;
  for (int side = 0; side < 2; ++side) {
    int v = scf_strip(s, side ? add->b : add->a);
    int selected = scf_strip(s, side ? add->a : add->b);
    const nyr_inst_t *sel = scf_def(s, selected);
    int cmp_value = -1;
    int64_t t = 0, f = 0, k = 0;
    if (!sel || sel->op != NYIR_SELECT_I64 ||
        !scf_const(s, sel->b, &t) || t < -16 || t > 16 ||
        !scf_const(s, sel->c, &f) || f < -24 || f > 24 ||
        f - t == 0 || f - t < -8 || f - t > 8 ||
        !scf_is_eq_const(s, sel->a, &cmp_value, &k) || cmp_value != v)
      continue;
    *prev = v;
    *key = k;
    *step = (int)t;
    *gap = (int)(f - t);
    return true;
  }
  return false;
}

static nyr_inst_t scf_new_inst(nyr_op_t op, int dst, int a, int b, int c,
                               int64_t imm) {
  return (nyr_inst_t){.op = op, .dst = dst, .a = a, .b = b, .c = c,
                      .d = -1, .e = -1, .f = -1, .imm = imm,
                      .scalar_class = NYIR_SCALAR_I64_RAW};
}

typedef struct {
  unsigned length;
  int root;
  int64_t key;
  int step, gap, extra;
} scf_chain_t;

static int scf_chain_extra(int gap) {
  int d = gap < 0 ? -gap : gap;
  return 8 + (d == 1 ? 0 : ((d & (d - 1)) == 0 ? 5 : 7));
}

static void scf_emit_chain(nyr_func_t *f, size_t pos, size_t dst_pc,
                           const scf_chain_t *chain) {
  int64_t steps = (int64_t)chain->length;
  int64_t width = (steps - 1) *
                  (int64_t)(chain->gap < 0 ? -chain->gap : chain->gap);
  int64_t lo = chain->gap > 0 ? chain->key - width : chain->key;
  int64_t hi = chain->gap > 0 ? chain->key : chain->key + width;
  int ids[15];
  for (int i = 0; i < chain->extra; ++i)
    ids[i] = f->next_value++;
  f->data[pos + 0] = scf_new_inst(NYIR_CONST_I64, ids[0], -1, -1, -1, lo);
  f->data[pos + 1] = scf_new_inst(NYIR_CONST_I64, ids[1], -1, -1, -1, hi);
  f->data[pos + 2] = scf_new_inst(NYIR_CMP_I64, ids[2], chain->root, ids[0],
                                  -1, 0);
  f->data[pos + 2].cmp = NYIR_CMP_GE;
  f->data[pos + 3] = scf_new_inst(NYIR_CMP_I64, ids[3], chain->root, ids[1],
                                  -1, 0);
  f->data[pos + 3].cmp = NYIR_CMP_LE;
  f->data[pos + 4] = scf_new_inst(NYIR_AND_I64, ids[4], ids[2], ids[3],
                                  -1, 0);
  f->data[pos + 5] = scf_new_inst(NYIR_CONST_I64, ids[5], -1, -1, -1,
                       steps * ((int64_t)chain->step + chain->gap));
  f->data[pos + 6] = scf_new_inst(NYIR_ADD_I64, ids[6], chain->root, ids[5],
                                  -1, 0);
  f->data[pos + 7] = scf_new_inst(NYIR_CONST_I64, ids[7], -1, -1, -1,
                                  chain->key + steps * (int64_t)chain->step);
  int cond = ids[4];
  if (chain->extra > 8) {
    int64_t modulus = chain->gap < 0 ? -chain->gap : chain->gap;
    if ((modulus & (modulus - 1)) == 0) {
      int64_t mask = modulus - 1;
      f->data[pos + 8] = scf_new_inst(NYIR_CONST_I64, ids[8], -1, -1, -1,
                                      mask);
      f->data[pos + 9] = scf_new_inst(NYIR_AND_I64, ids[9], chain->root,
                                      ids[8], -1, 0);
      f->data[pos + 10] = scf_new_inst(NYIR_CONST_I64, ids[10], -1, -1, -1,
                                       chain->key & mask);
      f->data[pos + 11] = scf_new_inst(NYIR_CMP_I64, ids[11], ids[9],
                                       ids[10], -1, 0);
      f->data[pos + 11].cmp = NYIR_CMP_EQ;
      f->data[pos + 12] = scf_new_inst(NYIR_AND_I64, ids[12], ids[4],
                                       ids[11], -1, 0);
      cond = ids[12];
    } else {
      f->data[pos + 8] = scf_new_inst(NYIR_CONST_I64, ids[8], -1, -1, -1,
                                      chain->key);
      f->data[pos + 9] = scf_new_inst(NYIR_SUB_I64, ids[9],
                         chain->gap > 0 ? ids[8] : chain->root,
                         chain->gap > 0 ? chain->root : ids[8], -1, 0);
      f->data[pos + 10] = scf_new_inst(NYIR_CONST_I64, ids[10], -1, -1, -1,
                                       modulus);
      f->data[pos + 11] = scf_new_inst(NYIR_MOD_I64, ids[11], ids[9],
                                       ids[10], -1, 0);
      f->data[pos + 12] = scf_new_inst(NYIR_CONST_I64, ids[12], -1, -1, -1, 0);
      f->data[pos + 13] = scf_new_inst(NYIR_CMP_I64, ids[13], ids[11],
                                       ids[12], -1, 0);
      f->data[pos + 13].cmp = NYIR_CMP_EQ;
      f->data[pos + 14] = scf_new_inst(NYIR_AND_I64, ids[14], ids[4],
                                       ids[13], -1, 0);
      cond = ids[14];
    }
  }
  nyr_inst_t *dst = &f->data[dst_pc];
  nir_make_copy(dst, cond);
  dst->op = NYIR_SELECT_I64;
  dst->a = cond;
  dst->b = ids[7];
  dst->c = ids[6];
}

static bool scf_fold_step_chain(nyr_func_t *f) {
  if (f->next_value <= 0)
    return true;
  scf_t s = {.f = f, .defs = nyr_build_defs(f)};
  size_t n = (size_t)f->next_value;
  unsigned *length = calloc(n, sizeof(*length));
  int *roots = malloc(n * sizeof(*roots));
  int64_t *keys = malloc(n * sizeof(*keys));
  int *increments = malloc(n * sizeof(*increments));
  int *gaps = malloc(n * sizeof(*gaps));
  unsigned char *dups = calloc(n, sizeof(*dups));
  unsigned char *extended = calloc(n, sizeof(*extended));
  scf_chain_t *candidates = calloc(f->len, sizeof(*candidates));
  if (!s.defs || !length || !roots || !keys || !increments || !gaps ||
      !dups || !extended || !candidates) {
    free(s.defs);
    free(length);
    free(roots);
    free(keys);
    free(increments);
    free(gaps);
    free(dups);
    free(extended);
    free(candidates);
    return true;
  }
  for (size_t i = 0; i < f->len; ++i)
    if (f->data[i].dst >= 0 && f->data[i].dst < f->next_value &&
        dups[f->data[i].dst] < 2)
      ++dups[f->data[i].dst];
  s.def_counts = dups;
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    int prev = -1;
    int64_t k = 0;
    int step = 0, gap = 0;
    if (in->dst < 0 || in->dst >= f->next_value ||
        (in->scalar_class != NYIR_SCALAR_UNKNOWN &&
         in->scalar_class != NYIR_SCALAR_I64_RAW) ||
        dups[in->dst] != 1 ||
        !scf_step_add(&s, in, &prev, &k, &step, &gap) || prev < 0 ||
        prev >= f->next_value || dups[prev] > 1 ||
        (s.defs[prev] >= 0 && (size_t)s.defs[prev] >= i))
      continue;
    unsigned count = 1;
    int root = prev;
    int64_t first = k;
    int64_t advance = (int64_t)length[prev] * (int64_t)step;
    if (length[prev] && length[prev] < 4096 &&
        increments[prev] == step && gaps[prev] == gap &&
        (advance >= 0 ? keys[prev] <= INT64_MAX - advance
                      : keys[prev] >= INT64_MIN - advance) &&
        k == keys[prev] + advance) {
      count += length[prev];
      root = roots[prev];
      first = keys[prev];
      extended[prev] = 1;
    }
    length[in->dst] = count;
    roots[in->dst] = root;
    keys[in->dst] = first;
    increments[in->dst] = step;
    gaps[in->dst] = gap;
  }
  size_t extra = 0;
  for (size_t v = 0; v < n; ++v) {
    if (length[v] < 8 || extended[v] || s.defs[v] < 0)
      continue;
    unsigned count = length[v];
    int64_t first = keys[v];
    if (first < INT64_MIN + 64 * (int64_t)count ||
        first > INT64_MAX - 64 * (int64_t)count)
      continue;
    int need = scf_chain_extra(gaps[v]);
    if ((size_t)need > (size_t)INT_MAX - (size_t)f->next_value ||
        extra > (size_t)INT_MAX - (size_t)f->next_value - (size_t)need ||
        (size_t)need > SIZE_MAX - f->len ||
        extra > SIZE_MAX - f->len - (size_t)need) {
      extra = 0;
      break;
    }
    candidates[s.defs[v]] = (scf_chain_t){.length = count, .root = roots[v],
            .key = first, .step = increments[v], .gap = gaps[v], .extra = need};
    extra += (size_t)need;
  }
  free(s.defs);
  free(length);
  free(roots);
  free(keys);
  free(increments);
  free(gaps);
  free(dups);
  free(extended);
  if (!extra) {
    free(candidates);
    return true;
  }
  if (!nir_ensure_inst_space(f, extra)) {
    free(candidates);
    return false;
  }
  size_t shift = extra;
  for (size_t pc = f->len; pc-- > 0;) {
    size_t dst = pc + shift;
    f->data[dst] = f->data[pc];
    const scf_chain_t *c = &candidates[pc];
    if (c->length) {
      shift -= (size_t)c->extra;
      scf_emit_chain(f, pc + shift, dst, c);
    }
  }
  f->len += extra;
  free(candidates);
  nyr_refresh_metadata(f);
  return true;
}

static bool scf_is_bool01(const nyr_func_t *f, const int *defs, int v,
                          unsigned depth) {
  if (!f || !defs || v < 0 || v >= f->next_value || defs[v] < 0 || depth > 12)
    return false;
  const nyr_inst_t *d = &f->data[(size_t)defs[v]];
  if (d->op == NYIR_CMP_I64 || d->op == NYIR_CMP_F64 || d->op == NYIR_CMP_F32)
    return true;
  if (d->op == NYIR_CONST_I64)
    return d->imm == 0 || d->imm == 1;
  if (d->op == NYIR_COPY)
    return scf_is_bool01(f, defs, d->a, depth + 1);
  if (d->op == NYIR_AND_I64 || d->op == NYIR_OR_I64 || d->op == NYIR_XOR_I64)
    return scf_is_bool01(f, defs, d->a, depth + 1) &&
           scf_is_bool01(f, defs, d->b, depth + 1);
  if (d->op == NYIR_SELECT_I64)
    return scf_is_bool01(f, defs, d->b, depth + 1) &&
           scf_is_bool01(f, defs, d->c, depth + 1);
  return false;
}

static int scf_strip_copy_defs(const nyr_func_t *f, const int *defs, int v) {
  for (unsigned d = 0; d < 16 && v >= 0 && v < f->next_value && defs[v] >= 0;
       ++d) {
    if (f->data[(size_t)defs[v]].op != NYIR_COPY)
      break;
    v = f->data[(size_t)defs[v]].a;
  }
  return v;
}

/*
 * Simplify boolean selects emitted by if-conversion:
 *   select(b0, b1, 0)       -> and(b0, b1)   when b0, b1 in {0, 1}
 *   select(b0, x + 1, x)    -> add(x, b0)    when b0 in {0, 1}
 */
static bool scf_fold_bool_selects(nyr_func_t *f) {
  if (!f || !f->len || f->next_value <= 0)
    return true;
  int *defs = nyr_build_defs(f);
  if (!defs)
    return false;
  bool changed = false;
  for (size_t i = 0; i < f->len; ++i) {
    nyr_inst_t *in = &f->data[i];
    if (in->op != NYIR_SELECT_I64 || in->dst < 0)
      continue;
    if (!scf_is_bool01(f, defs, in->a, 0))
      continue;
    int vb = scf_strip_copy_defs(f, defs, in->b);
    int vc = scf_strip_copy_defs(f, defs, in->c);
    if (vc >= 0 && vc < f->next_value && defs[vc] >= 0 &&
        f->data[(size_t)defs[vc]].op == NYIR_CONST_I64 &&
        f->data[(size_t)defs[vc]].imm == 0 && scf_is_bool01(f, defs, vb, 0)) {
      in->op = NYIR_AND_I64;
      in->b = vb;
      in->c = -1;
      in->scalar_class = NYIR_SCALAR_I64_RAW;
      in->effects = nyr_inst_effects(in);
      nyr_clear_derived_facts(in);
      changed = true;
      continue;
    }
    if (vb >= 0 && vb < f->next_value && defs[vb] >= 0) {
      const nyr_inst_t *add = &f->data[(size_t)defs[vb]];
      if (add->op == NYIR_ADD_I64 &&
          add->scalar_class == NYIR_SCALAR_I64_RAW) {
        int aa = scf_strip_copy_defs(f, defs, add->a);
        int ab = scf_strip_copy_defs(f, defs, add->b);
        bool matches =
            (aa == vc && ab >= 0 && ab < f->next_value && defs[ab] >= 0 &&
             f->data[(size_t)defs[ab]].op == NYIR_CONST_I64 &&
             f->data[(size_t)defs[ab]].imm == 1) ||
            (ab == vc && aa >= 0 && aa < f->next_value && defs[aa] >= 0 &&
             f->data[(size_t)defs[aa]].op == NYIR_CONST_I64 &&
             f->data[(size_t)defs[aa]].imm == 1);
        if (matches) {
          int cond = in->a;
          in->op = NYIR_ADD_I64;
          in->a = in->c;
          in->b = cond;
          in->c = -1;
          in->scalar_class = NYIR_SCALAR_I64_RAW;
          in->effects = nyr_inst_effects(in);
          nyr_clear_derived_facts(in);
          changed = true;
          continue;
        }
      }
    }
  }
  free(defs);
  if (changed)
    nyr_refresh_metadata(f);
  return true;
}

bool nyr_select_chain_fold(nyr_func_t *f) {
  if (!f || !f->len || getenv("NYTRIX_NO_SELECT_CHAIN_FOLD"))
    return true;
  /*
   * Keep boolean-delta selects intact until recurrence folding has matched
   * the full chain.
   */
  if (!scf_fold_step_chain(f))
    return false;
  if (!scf_fold_bool_selects(f))
    return false;
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
    if (isnan(x))
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
