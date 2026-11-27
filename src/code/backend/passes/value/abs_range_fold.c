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

static bool arf_hoist_disabled(void) {
  static int disabled = -1;
  if (disabled < 0) {
    const char *env = getenv("NYTRIX_NO_ABS_RANGE_HOIST");
    disabled = env && *env && strcmp(env, "0") != 0;
  }
  return disabled != 0;
}

static bool arf_proven_disabled(void) {
  static int disabled = -1;
  if (disabled < 0) {
    const char *env = getenv("NYTRIX_NO_ABS_RANGE_PROVEN");
    disabled = env && *env && strcmp(env, "0") != 0;
  }
  return disabled != 0;
}

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

static bool arf_is_mult8(const nyr_func_t *f, const int *defs, int v, int depth,
                         const int *vis_phi, size_t vis_len) {
  if (depth >= 8 || v < 0 || v >= f->next_value || defs[v] < 0)
    return false;
  const nyr_inst_t *in = &f->data[(size_t)defs[v]];
  int64_t c = 0;
  switch (in->op) {
  case NYIR_CONST_I64:
    return (in->imm & 7) == 0;
  case NYIR_COPY:
    return arf_is_mult8(f, defs, in->a, depth + 1, vis_phi, vis_len);
  case NYIR_SHL_I64:
    return arf_const(f, defs, in->b, &c) && c >= 3 && c < 64;
  case NYIR_MUL_I64:
  case NYIR_AND_I64:
    return (arf_const(f, defs, in->a, &c) && (c & 7) == 0) ||
           (arf_const(f, defs, in->b, &c) && (c & 7) == 0);
  case NYIR_ADD_I64:
  case NYIR_SUB_I64:
    return arf_is_mult8(f, defs, in->a, depth + 1, vis_phi, vis_len) &&
           arf_is_mult8(f, defs, in->b, depth + 1, vis_phi, vis_len);
  case NYIR_PHI: {
    for (size_t i = 0; i < vis_len; ++i)
      if (vis_phi[i] == v)
        return true;
    if (vis_len >= 8 || !in->phi_incoming_len || !in->phi_incoming)
      return false;
    int next_vis[8];
    for (size_t i = 0; i < vis_len; ++i)
      next_vis[i] = vis_phi[i];
    next_vis[vis_len] = v;
    for (size_t i = 0; i < in->phi_incoming_len; ++i) {
      if (!arf_is_mult8(f, defs, in->phi_incoming[i].value, depth + 1, next_vis,
                        vis_len + 1))
        return false;
    }
    return true;
  }
  default:
    return false;
  }
}

static void arf_intersect_lo(bool *has_lo, int64_t *lo, int64_t cand) {
  if (!*has_lo || cand > *lo) {
    *has_lo = true;
    *lo = cand;
  }
}

static void arf_intersect_hi(bool *has_hi, int64_t *hi, int64_t cand) {
  if (!*has_hi || cand < *hi) {
    *has_hi = true;
    *hi = cand;
  }
}

static void arf_val_bounds(const nyr_func_t *f, const int *defs,
                           const nyr_use_def_t *uses, int v, int depth,
                           bool allow_alloca, bool *out_hlo, int64_t *out_lo,
                           bool *out_hhi, int64_t *out_hi);

static bool arf_find_zero_memset(const nyr_func_t *f, const int *defs, int root,
                                 size_t root_def, int64_t bytes,
                                 size_t *out_idx) {
  for (size_t i = root_def + 1; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->op == NYIR_CALL && in->imm == 3 && !in->extra_args_len &&
        in->symbol && strcmp(in->symbol, "rt_memset") == 0 && in->a == root) {
      int64_t fill = 1, sz = 0;
      if (in->dst != root && arf_const(f, defs, in->b, &fill) && fill == 0 &&
          arf_const(f, defs, in->c, &sz) && sz == bytes) {
        *out_idx = i;
        return true;
      }
      return false;
    }
    if (in->op == NYIR_LABEL || in->op == NYIR_BR || in->op == NYIR_BR_IF ||
        in->op == NYIR_RET || in->op == NYIR_CALL)
      return false;
    if (in->effects & (NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_CALL |
                       NYIR_EFFECT_VOLATILE | NYIR_EFFECT_UNKNOWN_SIDE_EFFECT))
      return false;
    if (in->a == root || in->b == root || in->c == root || in->d == root ||
        in->e == root || in->f == root)
      return false;
  }
  return false;
}

static bool arf_alloca_i64_bounds(const nyr_func_t *f, const int *defs,
                                  const nyr_use_def_t *uses, int root,
                                  int64_t *out_lo, int64_t *out_hi) {
  if (!uses || !uses->offsets || root < 0 || root >= f->next_value ||
      (size_t)root >= uses->value_count || defs[root] < 0)
    return false;
  size_t root_def = (size_t)defs[root];
  const nyr_inst_t *alloc = &f->data[root_def];
  if (alloc->op != NYIR_ALLOCA || alloc->imm <= 0 || (alloc->imm % 8) != 0)
    return false;
  size_t memset_idx = 0;
  if (!arf_find_zero_memset(f, defs, root, root_def, alloc->imm, &memset_idx))
    return false;

  bool result = false;
  size_t nv = (size_t)f->next_value;
  bool *is_ptr = calloc(nv, sizeof(*is_ptr));
  int *queue = malloc(nv * sizeof(*queue));
  size_t *stores = NULL;
  size_t store_n = 0, store_cap = 0;
  if (!is_ptr || !queue)
    goto done;

  size_t qhead = 0, qtail = 0;
  is_ptr[root] = true;
  queue[qtail++] = root;

  while (qhead < qtail) {
    int p = queue[qhead++];
    if ((size_t)p >= uses->value_count)
      goto done;
    size_t ub = uses->offsets[p];
    size_t ue = uses->offsets[p + 1];
    for (size_t ui = ub; ui < ue; ++ui) {
      size_t u = uses->users[ui];
      if (u >= f->len)
        goto done;
      const nyr_inst_t *in = &f->data[u];
      if (in->op == NYIR_NOP)
        continue;
      if (u == memset_idx) {
        if (in->a != root || in->b == p || in->c == p)
          goto done;
        continue;
      }
      if (in->op == NYIR_COPY) {
        if (in->a != p || in->dst < 0 || in->dst >= f->next_value)
          goto done;
        if (!is_ptr[in->dst]) {
          is_ptr[in->dst] = true;
          queue[qtail++] = in->dst;
        }
        continue;
      }
      if (in->op == NYIR_ADD_I64) {
        int other = -1;
        if (in->a == p && in->b != p)
          other = in->b;
        else if (in->b == p && in->a != p)
          other = in->a;
        else
          goto done;
        if (in->dst < 0 || in->dst >= f->next_value ||
            !arf_is_mult8(f, defs, other, 0, NULL, 0))
          goto done;
        if (!is_ptr[in->dst]) {
          is_ptr[in->dst] = true;
          queue[qtail++] = in->dst;
        }
        continue;
      }
      if (in->op == NYIR_SUB_I64) {
        if (in->a != p || in->b == p || in->dst < 0 ||
            in->dst >= f->next_value ||
            !arf_is_mult8(f, defs, in->b, 0, NULL, 0))
          goto done;
        if (!is_ptr[in->dst]) {
          is_ptr[in->dst] = true;
          queue[qtail++] = in->dst;
        }
        continue;
      }
      if (in->op == NYIR_LOAD_I64) {
        if (in->flags != 0 || in->a != p ||
            (in->b >= 0 &&
             (in->b == p || !arf_is_mult8(f, defs, in->b, 0, NULL, 0))))
          goto done;
        continue;
      }
      if (in->op == NYIR_STORE_I64) {
        if (in->flags != 0 || in->a != p || in->c < 0 || in->c == p ||
            (in->b >= 0 &&
             (in->b == p || !arf_is_mult8(f, defs, in->b, 0, NULL, 0))))
          goto done;
        if (store_n == store_cap) {
          size_t nc = store_cap ? store_cap * 2 : 16;
          size_t *nd = realloc(stores, nc * sizeof(*nd));
          if (!nd)
            goto done;
          stores = nd;
          store_cap = nc;
        }
        stores[store_n++] = u;
        continue;
      }
      goto done;
    }
  }

  /*
   * Soundness: no pointer derived from root may be stored or escape.
   */
  int64_t lo = 0, hi = 0;
  for (size_t si = 0; si < store_n; ++si) {
    const nyr_inst_t *st = &f->data[stores[si]];
    if (st->c < 0 || st->c >= f->next_value || is_ptr[st->c])
      goto done;
    bool shlo = false, shhi = false;
    int64_t slo = 0, shi = 0;
    arf_val_bounds(f, defs, uses, st->c, 0, false, &shlo, &slo, &shhi, &shi);
    if (!shlo || !shhi)
      goto done;
    if (slo < lo)
      lo = slo;
    if (shi > hi)
      hi = shi;
  }
  *out_lo = lo;
  *out_hi = hi;
  result = true;

done:
  free(is_ptr);
  free(queue);
  free(stores);
  return result;
}

static bool arf_match_select_clamp(const nyr_func_t *f, const int *defs,
                                   const nyr_use_def_t *uses,
                                   const nyr_inst_t *sel, int depth,
                                   bool allow_alloca, bool *out_hlo,
                                   int64_t *out_lo, bool *out_hhi,
                                   int64_t *out_hi) {
  int cmp_v = nyr_strip_copy_chain(f, defs, sel->a, 8);
  if (cmp_v < 0 || cmp_v >= f->next_value || defs[cmp_v] < 0)
    return false;
  const nyr_inst_t *cmp = &f->data[(size_t)defs[cmp_v]];
  if (cmp->op != NYIR_CMP_I64)
    return false;
  int ca = nyr_strip_copy_chain(f, defs, cmp->a, 8);
  int cb = nyr_strip_copy_chain(f, defs, cmp->b, 8);

  for (int pass = 0; pass < 2; ++pass) {
    int cand_y = pass == 0 ? sel->c : sel->b;
    int cand_c = pass == 0 ? sel->b : sel->c;
    bool true_is_const = pass == 0;
    int y = nyr_strip_copy_chain(f, defs, cand_y, 8);
    int64_t cval = 0, cval2 = 0;
    if (y < 0 || !arf_const(f, defs, cand_c, &cval))
      continue;
    nyr_cmp_t pred;
    if (ca == y && arf_const(f, defs, cb, &cval2) && cval2 == cval) {
      pred = cmp->cmp;
    } else if (cb == y && arf_const(f, defs, ca, &cval2) && cval2 == cval) {
      pred = nyr_cmp_swap_operands(cmp->cmp);
    } else {
      continue;
    }
    bool is_max =
        (true_is_const && (pred == NYIR_CMP_LT || pred == NYIR_CMP_LE)) ||
        (!true_is_const && (pred == NYIR_CMP_GT || pred == NYIR_CMP_GE));
    bool is_min =
        (true_is_const && (pred == NYIR_CMP_GT || pred == NYIR_CMP_GE)) ||
        (!true_is_const && (pred == NYIR_CMP_LT || pred == NYIR_CMP_LE));
    if (!is_max && !is_min)
      continue;
    bool yhlo = false, yhhi = false;
    int64_t ylo = 0, yhi = 0;
    arf_val_bounds(f, defs, uses, y, depth + 1, allow_alloca, &yhlo, &ylo,
                   &yhhi, &yhi);
    if (is_max) {
      *out_hlo = true;
      *out_lo = (yhlo && ylo > cval) ? ylo : cval;
      if (yhhi) {
        *out_hhi = true;
        *out_hi = yhi > cval ? yhi : cval;
      }
    } else {
      *out_hhi = true;
      *out_hi = (yhhi && yhi < cval) ? yhi : cval;
      if (yhlo) {
        *out_hlo = true;
        *out_lo = ylo < cval ? ylo : cval;
      }
    }
    return true;
  }
  return false;
}

static int arf_trace_alloca_root(const nyr_func_t *f, const int *defs, int ptr) {
  int cur = ptr;
  for (int step = 0; step < 16 && cur >= 0 && cur < f->next_value; ++step) {
    if (defs[cur] < 0)
      return -1;
    const nyr_inst_t *d = &f->data[(size_t)defs[cur]];
    if (d->op == NYIR_ALLOCA)
      return cur;
    if (d->op == NYIR_COPY) {
      cur = d->a;
      continue;
    }
    if (d->op == NYIR_ADD_I64) {
      if (arf_is_mult8(f, defs, d->b, 0, NULL, 0)) {
        cur = d->a;
        continue;
      }
      if (arf_is_mult8(f, defs, d->a, 0, NULL, 0)) {
        cur = d->b;
        continue;
      }
      return -1;
    }
    if (d->op == NYIR_SUB_I64) {
      if (arf_is_mult8(f, defs, d->b, 0, NULL, 0)) {
        cur = d->a;
        continue;
      }
      return -1;
    }
    return -1;
  }
  return -1;
}

static void arf_val_bounds(const nyr_func_t *f, const int *defs,
                           const nyr_use_def_t *uses, int v, int depth,
                           bool allow_alloca, bool *out_hlo, int64_t *out_lo,
                           bool *out_hhi, int64_t *out_hi) {
  *out_hlo = false;
  *out_hhi = false;
  *out_lo = 0;
  *out_hi = 0;
  if (depth >= 8 || v < 0 || v >= f->next_value || defs[v] < 0)
    return;
  const nyr_inst_t *in = &f->data[(size_t)defs[v]];
  if (in->range.has_min)
    arf_intersect_lo(out_hlo, out_lo, in->range.min);
  if (in->range.has_max)
    arf_intersect_hi(out_hhi, out_hi, in->range.max);

  switch (in->op) {
  case NYIR_CONST_I64:
    arf_intersect_lo(out_hlo, out_lo, in->imm);
    arf_intersect_hi(out_hhi, out_hi, in->imm);
    break;
  case NYIR_COPY: {
    bool ahlo = false, ahhi = false;
    int64_t alo = 0, ahi = 0;
    arf_val_bounds(f, defs, uses, in->a, depth + 1, allow_alloca, &ahlo, &alo,
                   &ahhi, &ahi);
    if (ahlo)
      arf_intersect_lo(out_hlo, out_lo, alo);
    if (ahhi)
      arf_intersect_hi(out_hhi, out_hi, ahi);
    break;
  }
  case NYIR_AND_I64: {
    int64_t m = -1;
    if ((arf_const(f, defs, in->b, &m) && m >= 0) ||
        (arf_const(f, defs, in->a, &m) && m >= 0)) {
      arf_intersect_lo(out_hlo, out_lo, 0);
      arf_intersect_hi(out_hhi, out_hi, m);
    }
    break;
  }
  case NYIR_SELECT_I64: {
    bool shlo = false, shhi = false;
    int64_t slo = 0, shi = 0;
    if (arf_match_select_clamp(f, defs, uses, in, depth, allow_alloca, &shlo,
                               &slo, &shhi, &shi)) {
      if (shlo)
        arf_intersect_lo(out_hlo, out_lo, slo);
      if (shhi)
        arf_intersect_hi(out_hhi, out_hi, shi);
    }
    break;
  }
  case NYIR_ADD_I64:
  case NYIR_SUB_I64: {
    bool ahlo = false, ahhi = false, bhlo = false, bhhi = false;
    int64_t alo = 0, ahi = 0, blo = 0, bhi = 0;
    arf_val_bounds(f, defs, uses, in->a, depth + 1, allow_alloca, &ahlo, &alo,
                   &ahhi, &ahi);
    arf_val_bounds(f, defs, uses, in->b, depth + 1, allow_alloca, &bhlo, &blo,
                   &bhhi, &bhi);
    if (ahlo && ahhi && bhlo && bhhi) {
      ny_i128 rlo =
          in->op == NYIR_ADD_I64 ? (ny_i128)alo + blo : (ny_i128)alo - bhi;
      ny_i128 rhi =
          in->op == NYIR_ADD_I64 ? (ny_i128)ahi + bhi : (ny_i128)ahi - blo;
      if (rlo >= INT64_MIN && rhi <= INT64_MAX) {
        arf_intersect_lo(out_hlo, out_lo, (int64_t)rlo);
        arf_intersect_hi(out_hhi, out_hi, (int64_t)rhi);
      }
    }
    break;
  }
  case NYIR_LOAD_I64: {
    if (!allow_alloca || in->flags != 0 ||
        (in->b >= 0 && !arf_is_mult8(f, defs, in->b, 0, NULL, 0)))
      break;
    int root = arf_trace_alloca_root(f, defs, in->a);
    int64_t alo = 0, ahi = 0;
    if (root >= 0 && arf_alloca_i64_bounds(f, defs, uses, root, &alo, &ahi)) {
      arf_intersect_lo(out_hlo, out_lo, alo);
      arf_intersect_hi(out_hhi, out_hi, ahi);
    }
    break;
  }
  default:
    break;
  }
}

static bool arf_not_int64_min(const nyr_func_t *f, const int *defs,
                              const nyr_use_def_t *uses, int v) {
  bool hlo = false, hhi = false;
  int64_t lo = 0, hi = 0;
  arf_val_bounds(f, defs, uses, v, 0, true, &hlo, &lo, &hhi, &hi);
  return hlo && lo > INT64_MIN;
}

/*
 * a == select(slt x 0, sub 0 x, x): returns x, or -1.
 */
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

static nyr_inst_t arf_make_const(int dst, int64_t imm, nyr_debug_loc_t debug) {
  nyr_inst_t in = {.op = NYIR_CONST_I64,
                   .dst = dst,
                   .a = -1,
                   .b = -1,
                   .c = -1,
                   .d = -1,
                   .e = -1,
                   .f = -1,
                   .imm = imm,
                   .debug = debug};
  in.effects = nyr_inst_effects(&in);
  return in;
}

static nyr_inst_t arf_make_binop(nyr_op_t op, int dst, int a, int b,
                                 nyr_debug_loc_t debug) {
  nyr_inst_t in = {.op = op,
                   .dst = dst,
                   .a = a,
                   .b = b,
                   .c = -1,
                   .d = -1,
                   .e = -1,
                   .f = -1,
                   .debug = debug,
                   .scalar_class = NYIR_SCALAR_I64_RAW};
  in.effects = nyr_inst_effects(&in);
  return in;
}

typedef struct {
  size_t at;
  int x;
  int64_t k, l;
} arf_site_t;

typedef struct {
  int val;
  int64_t k;
  bool is_sub;
  int biased_v;
} arf_hoist_key_t;

typedef struct {
  size_t pos;
  size_t seq;
  size_t count;
  nyr_inst_t insts[3];
} arf_ins_t;

static int arf_ins_cmp(const void *pa, const void *pb) {
  const arf_ins_t *a = (const arf_ins_t *)pa;
  const arf_ins_t *b = (const arf_ins_t *)pb;
  if (a->pos != b->pos)
    return a->pos < b->pos ? -1 : 1;
  if (a->seq != b->seq)
    return a->seq < b->seq ? -1 : 1;
  return 0;
}

/*
 * Check if x == a - b inside site_blk where exactly one operand is defined in
 * a strictly dominating predecessor block. Sets *out_inv, *out_var, and
 * *out_is_sub (`false` -> `(a + K) - b`, `true` -> `a - (b - K)`).
 */
static bool arf_match_hoist(const nyr_func_t *f, const nyr_cfg_t *cfg,
                            const int *defs, size_t at, int x, int *out_inv,
                            int *out_var, size_t *out_sub_def,
                            bool *out_is_sub) {
  if (arf_hoist_disabled() || !cfg || !cfg->inst_block || at >= f->len)
    return false;
  int xv = nyr_strip_copy_chain(f, defs, x, 16);
  if (xv < 0 || xv >= f->next_value || defs[xv] < 0)
    return false;
  size_t sub_def = (size_t)defs[xv];
  const nyr_inst_t *sub = &f->data[sub_def];
  if (sub->op != NYIR_SUB_I64 || sub->a < 0 || sub->b < 0)
    return false;
  int a = nyr_strip_copy_chain(f, defs, sub->a, 16);
  int b = nyr_strip_copy_chain(f, defs, sub->b, 16);
  if (a < 0 || a >= f->next_value || defs[a] < 0 || b < 0 ||
      b >= f->next_value || defs[b] < 0)
    return false;
  size_t site_blk = cfg->inst_block[at];
  size_t sub_blk = cfg->inst_block[sub_def];
  size_t a_blk = cfg->inst_block[(size_t)defs[a]];
  size_t b_blk = cfg->inst_block[(size_t)defs[b]];
  if (site_blk >= cfg->block_count || sub_blk >= cfg->block_count ||
      a_blk >= cfg->block_count || b_blk >= cfg->block_count)
    return false;
  if (cfg->reachable && !cfg->reachable[site_blk])
    return false;
  if (sub_blk == site_blk && b_blk == site_blk && a_blk != site_blk &&
      nyr_cfg_dominates(cfg, a_blk, site_blk)) {
    *out_inv = a;
    *out_var = b;
    *out_sub_def = sub_def;
    *out_is_sub = false;
    return true;
  }
  if (sub_blk == site_blk && a_blk == site_blk && b_blk != site_blk &&
      nyr_cfg_dominates(cfg, b_blk, site_blk)) {
    *out_inv = b;
    *out_var = a;
    *out_sub_def = sub_def;
    *out_is_sub = true;
    return true;
  }
  return false;
}

static bool arf_push_ins(arf_ins_t **arr, size_t *len, size_t *cap,
                         size_t *total_extra, arf_ins_t item) {
  if (*len == *cap) {
    size_t nc = *cap ? *cap * 2 : 16;
    arf_ins_t *nd = realloc(*arr, nc * sizeof(*nd));
    if (!nd)
      return false;
    *arr = nd;
    *cap = nc;
  }
  item.seq = *len;
  (*arr)[(*len)++] = item;
  *total_extra += item.count;
  return true;
}

bool nyr_abs_range_fold(nyr_func_t *f) {
  static int disabled = -1;
  if (disabled < 0) {
    const char *env = getenv("NYTRIX_NO_ABS_RANGE_FOLD");
    disabled = env && *env && strcmp(env, "0") != 0;
  }
  if (disabled || !f || (!f->untagged_values && arf_proven_disabled()) ||
      f->len < 4 || f->next_value <= 0)
    return true;
  int *defs = nyr_build_defs(f);
  if (!defs)
    return false;
  nyr_use_def_t uses = {0};
  bool uses_built = false;
  /*
   * Collect rewrites first; inserting shifts indices.
   */
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
    if (!f->untagged_values) {
      if (!uses_built) {
        if (!nyr_build_use_def(f, &uses)) {
          free(sites);
          free(defs);
          return false;
        }
        uses_built = true;
      }
      if (!arf_not_int64_min(f, defs, &uses, x))
        continue;
    }
    if (n == cap) {
      size_t nc = cap ? cap * 2 : 16;
      arf_site_t *nd = realloc(sites, nc * sizeof(*nd));
      if (!nd) {
        if (uses_built)
          nyr_use_def_free(&uses);
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
  if (uses_built)
    nyr_use_def_free(&uses);
  free(defs);
  if (!n) {
    free(sites);
    return true;
  }

  bool ok = false;
  nyr_func_t work = {0};
  nyr_cfg_t cfg = {0};
  int *wdefs = NULL;
  arf_ins_t *ins = NULL;
  size_t ins_n = 0, ins_cap = 0, total_extra = 0;
  arf_hoist_key_t *hoists = NULL;
  size_t hoist_n = 0, hoist_cap = 0;

  if (!nyr_ir_func_clone(f, &work))
    goto out;
  if (!nyr_cfg_build(&work, &cfg))
    goto out;
  wdefs = nyr_build_defs(&work);
  if (!wdefs)
    goto out;

  for (size_t s = 0; s < n; ++s) {
    size_t at = sites[s].at;
    nyr_inst_t *cmp = &work.data[at];
    int inv_v = -1, var_v = -1;
    size_t sub_def = 0;
    bool is_sub = false;
    if (arf_match_hoist(&work, &cfg, wdefs, at, sites[s].x, &inv_v, &var_v,
                        &sub_def, &is_sub)) {
      int biased_v = -1;
      for (size_t h = 0; h < hoist_n; ++h) {
        if (hoists[h].val == inv_v && hoists[h].k == sites[s].k &&
            hoists[h].is_sub == is_sub) {
          biased_v = hoists[h].biased_v;
          break;
        }
      }
      if (biased_v < 0) {
        size_t inv_def = (size_t)wdefs[inv_v];
        size_t inv_blk = cfg.inst_block[inv_def];
        size_t insert_pos = inv_def + 1;
        while (insert_pos < cfg.block_end[inv_blk] &&
               (work.data[insert_pos].op == NYIR_PHI ||
                work.data[insert_pos].op == NYIR_NOP))
          ++insert_pos;
        int kc_v = work.next_value++;
        biased_v = work.next_value++;
        nyr_debug_loc_t inv_dbg = work.data[inv_def].debug;
        arf_ins_t h_ins = {
            .pos = insert_pos,
            .count = 2,
            .insts = {arf_make_const(kc_v, sites[s].k, inv_dbg),
                      arf_make_binop(is_sub ? NYIR_SUB_I64 : NYIR_ADD_I64,
                                     biased_v, inv_v, kc_v, inv_dbg)}};
        if (!arf_push_ins(&ins, &ins_n, &ins_cap, &total_extra, h_ins))
          goto out;
        if (hoist_n == hoist_cap) {
          size_t nc = hoist_cap ? hoist_cap * 2 : 8;
          arf_hoist_key_t *nd = realloc(hoists, nc * sizeof(*nd));
          if (!nd)
            goto out;
          hoists = nd;
          hoist_cap = nc;
        }
        hoists[hoist_n++] = (arf_hoist_key_t){
            .val = inv_v, .k = sites[s].k, .is_sub = is_sub, .biased_v = biased_v};
      }
      int lc_v = work.next_value++;
      int diff_v = work.next_value++;
      nyr_debug_loc_t cmp_dbg = cmp->debug;
      nyr_debug_loc_t sub_dbg = work.data[sub_def].debug;
      cmp->cmp = NYIR_CMP_LT;
      cmp->a = diff_v;
      cmp->b = lc_v;
      nyr_clear_derived_facts(cmp);
      /*
       * Keep diff_v adjacent to var_v's load so vector RHS load folding fires.
       */
      arf_ins_t d_ins = {
          .pos = sub_def,
          .count = 1,
          .insts = {arf_make_binop(NYIR_SUB_I64, diff_v,
                                   is_sub ? var_v : biased_v,
                                   is_sub ? biased_v : var_v, sub_dbg)}};
      arf_ins_t c_ins = {
          .pos = at,
          .count = 1,
          .insts = {arf_make_const(lc_v, sites[s].l, cmp_dbg)}};
      if (!arf_push_ins(&ins, &ins_n, &ins_cap, &total_extra, d_ins) ||
          !arf_push_ins(&ins, &ins_n, &ins_cap, &total_extra, c_ins))
        goto out;
    } else {
      int kc_v = work.next_value++;
      int lc_v = work.next_value++;
      int add_v = work.next_value++;
      nyr_debug_loc_t cmp_dbg = cmp->debug;
      cmp->cmp = NYIR_CMP_LT;
      cmp->a = add_v;
      cmp->b = lc_v;
      nyr_clear_derived_facts(cmp);
      arf_ins_t c_ins = {
          .pos = at,
          .count = 3,
          .insts = {arf_make_const(kc_v, sites[s].k, cmp_dbg),
                    arf_make_const(lc_v, sites[s].l, cmp_dbg),
                    arf_make_binop(NYIR_ADD_I64, add_v, sites[s].x, kc_v,
                                   cmp_dbg)}};
      if (!arf_push_ins(&ins, &ins_n, &ins_cap, &total_extra, c_ins))
        goto out;
    }
  }

  if (!nir_ensure_inst_space(&work, total_extra))
    goto out;
  qsort(ins, ins_n, sizeof(*ins), arf_ins_cmp);
  for (size_t k = ins_n; k-- > 0;) {
    size_t pos = ins[k].pos;
    size_t cnt = ins[k].count;
    memmove(&work.data[pos + cnt], &work.data[pos],
            (work.len - pos) * sizeof(*work.data));
    for (size_t j = 0; j < cnt; ++j)
      work.data[pos + j] = ins[k].insts[j];
    work.len += cnt;
  }

  if (!nyr_dce(&work))
    goto out;
  char err[256] = {0};
  if (nyr_ir_verify(&work, err, sizeof(err))) {
    nyr_func_replace(f, &work);
    ok = true;
  }

out:
  if (!ok)
    nyr_ir_func_free(&work);
  nyr_cfg_free(&cfg);
  free(wdefs);
  free(hoists);
  free(ins);
  free(sites);
  return true;
}
