/*
 * |x| < C  ->  one biased range compare.
 *
 *   c = slt x 0 ; n = sub 0 x ; a = select c n x ; t = slt a C     (C > 0)
 *
 * is `(x + (C - 1)) <u (2C - 1)`; NYIR has no unsigned compare, so with the
 * usual sign-bit bias it becomes
 *
 *   t = slt (add x K) L      K = C - 1 + INT64_MIN, L = 2C - 1 + INT64_MIN
 *
 * The identity differs only at x == INT64_MIN, where `-x` overflows. That is
 * undefined behaviour in C, so the fold runs only for C translation units
 * (untagged_values); Ny arithmetic wraps and keeps the select form. `a <= C`
 * folds as `a < C + 1`. The select itself is left for DCE when it has no other
 * use. clang emits the same compare for cd's pairwise overlap test, which
 * then vectorizes without the per-lane abs blends.
 * NYTRIX_NO_ABS_RANGE_FOLD=1 disables it.
 */
#include "code/backend/passes/util.h"
#include <stdlib.h>
#include <string.h>

static bool arf_const(const nyr_func_t *f, const int *defs, int v,
                      int64_t *out) {
  for (int depth = 0; depth < 8 && v >= 0 && v < f->next_value; ++depth) {
    int at = defs[v];
    if (at < 0)
      return false;
    const nyr_inst_t *d = &f->data[at];
    if (d->op == NYIR_CONST_I64) {
      *out = d->imm;
      return true;
    }
    if (d->op != NYIR_COPY || d->a < 0)
      return false;
    v = d->a;
  }
  return false;
}

/* a == select(slt x 0, sub 0 x, x): returns x, or -1. */
static int arf_abs_source(const nyr_func_t *f, const int *defs, int a) {
  if (a < 0 || a >= f->next_value || defs[a] < 0)
    return -1;
  const nyr_inst_t *sel = &f->data[defs[a]];
  if (sel->op != NYIR_SELECT_I64 || sel->a < 0 || sel->b < 0 || sel->c < 0)
    return -1;
  int x = sel->c;
  int64_t zero = 1;
  if (defs[sel->a] < 0 || defs[sel->b] < 0)
    return -1;
  const nyr_inst_t *cmp = &f->data[defs[sel->a]];
  const nyr_inst_t *neg = &f->data[defs[sel->b]];
  if (cmp->op != NYIR_CMP_I64 || cmp->cmp != NYIR_CMP_LT || cmp->a != x ||
      !arf_const(f, defs, cmp->b, &zero) || zero != 0)
    return -1;
  zero = 1;
  if (neg->op != NYIR_SUB_I64 || neg->b != x ||
      !arf_const(f, defs, neg->a, &zero) || zero != 0)
    return -1;
  return x;
}

bool nyr_abs_range_fold(nyr_func_t *f) {
  static int disabled = -1;
  if (disabled < 0) {
    const char *env = getenv("NYTRIX_NO_ABS_RANGE_FOLD");
    disabled = env && *env && strcmp(env, "0") != 0;
  }
  if (disabled || !f || !f->untagged_values || f->len < 4 ||
      f->next_value <= 0)
    return true;
  int *defs = nyr_build_defs(f);
  if (!defs)
    return false;
  /* Collect rewrites first; inserting shifts indices. */
  typedef struct {
    size_t at;
    int x;
    int64_t k, l;
  } arf_site_t;
  arf_site_t *sites = NULL;
  size_t n = 0, cap = 0;
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->op != NYIR_CMP_I64 || in->dst < 0 ||
        (in->cmp != NYIR_CMP_LT && in->cmp != NYIR_CMP_LE) ||
        (in->flags & NYIR_INST_F_ASSUME))
      continue;
    int64_t c = 0;
    if (!arf_const(f, defs, in->b, &c))
      continue;
    if (in->cmp == NYIR_CMP_LE) {
      if (c >= ((int64_t)1 << 60))
        continue;
      c += 1;
    }
    if (c <= 0 || c > ((int64_t)1 << 60))
      continue;
    int x = arf_abs_source(f, defs, in->a);
    if (x < 0)
      continue;
    if (n == cap) {
      size_t nc = cap ? cap * 2 : 16;
      arf_site_t *nd = realloc(sites, nc * sizeof(*nd));
      if (!nd) {
        free(sites);
        free(defs);
        return false;
      }
      sites = nd, cap = nc;
    }
    sites[n++] = (arf_site_t){
        .at = i,
        .x = x,
        .k = (int64_t)((uint64_t)(c - 1) + (uint64_t)INT64_MIN),
        .l = (int64_t)((uint64_t)(2 * c - 1) + (uint64_t)INT64_MIN)};
  }
  free(defs);
  if (!n) {
    free(sites);
    return true;
  }
  if (!nir_ensure_inst_space(f, 3 * n)) {
    free(sites);
    return false;
  }
  /* Back to front: each site gets K, L and `add x K` right before it. */
  for (size_t s = n; s-- > 0;) {
    size_t at = sites[s].at;
    nyr_inst_t *cmp = &f->data[at];
    nyr_inst_t kc = {.op = NYIR_CONST_I64, .dst = f->next_value++,
                     .a = -1, .b = -1, .c = -1, .d = -1, .e = -1, .f = -1,
                     .imm = sites[s].k, .debug = cmp->debug};
    nyr_inst_t lc = kc;
    lc.dst = f->next_value++;
    lc.imm = sites[s].l;
    nyr_inst_t add = {.op = NYIR_ADD_I64, .dst = f->next_value++,
                      .a = sites[s].x, .b = kc.dst, .c = -1, .d = -1,
                      .e = -1, .f = -1, .debug = cmp->debug,
                      .scalar_class = NYIR_SCALAR_I64_RAW};
    kc.effects = nyr_inst_effects(&kc);
    lc.effects = nyr_inst_effects(&lc);
    add.effects = nyr_inst_effects(&add);
    cmp->cmp = NYIR_CMP_LT;
    cmp->a = add.dst;
    cmp->b = lc.dst;
    nyr_clear_derived_facts(cmp);
    memmove(&f->data[at + 3], &f->data[at], (f->len - at) * sizeof(*f->data));
    f->data[at] = kc;
    f->data[at + 1] = lc;
    f->data[at + 2] = add;
    f->len += 3;
  }
  free(sites);
  return true;
}
