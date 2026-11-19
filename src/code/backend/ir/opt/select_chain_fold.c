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
 */
#include "code/backend/ir/opt/loop.h"
#include "code/backend/ir/opt/util.h"
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
  bool ok = nyr_cfg_natural_loop_blocks(cfg, latch, h, member,
                                        cfg->block_count) &&
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
    for (size_t k = 0; in->args && k < in->args_len; ++k)
      if (in->args[k] >= 0 && in->args[k] < f->next_value)
        s->uses[in->args[k]]++;
    for (size_t k = 0; in->phi_incoming && k < in->phi_incoming_len; ++k) {
      int v = in->phi_incoming[k].value;
      if (v >= 0 && v < f->next_value)
        s->uses[v]++;
    }
  }
  s->scev_ok = nyr_has_loop(f) && nyr_scev_analyze(f, &s->scev);
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
 * Replace an add's x operand (x = the select being bypassed).
 */
static bool scf_retarget_add(scf_t *s, int add, int x, int arm) {
  nyr_inst_t *d = &s->f->data[s->defs[add]];
  int64_t c = 0;
  if (scf_strip(s, d->a) == x && scf_const(s, d->b, &c))
    d->a = arm;
  else if (scf_strip(s, d->b) == x && scf_const(s, d->a, &c))
    d->b = arm;
  else
    return false;
  int64_t ac = 0;
  if (scf_const(s, arm, &ac))
    nir_make_const(d, (int64_t)((uint64_t)ac + (uint64_t)c));
  return true;
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
          nir_make_const(t, (int64_t)((uint64_t)k + (uint64_t)c));
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
          if (scf_retarget_add(&s, av, x, arm ? xd->c : xd->b)) {
            s.uses[x]--;
            *changed = true;
          }
          break;
        }
      }
      continue;
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
      if (ra == rb) {
        nir_make_const(in, ra);
        *changed = true;
      } else if (ra == 1) {
        /*
         * A boolean compare result is 0/1, like the select condition.
         */
        const nyr_inst_t *cd = scf_def(&s, scf_strip(&s, sd->a));
        if (cd && cd->op == NYIR_CMP_I64) {
          nir_make_copy(in, scf_strip(&s, sd->a));
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
