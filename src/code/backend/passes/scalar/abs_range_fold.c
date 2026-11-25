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
  if (disabled || !f || !f->untagged_values || f->len < 4 ||
      f->next_value <= 0)
    return true;
  int *defs = nyr_build_defs(f);
  if (!defs)
    return false;
  /* Collect rewrites first; inserting shifts indices. */
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
      /* Keep diff_v adjacent to var_v's load so vector RHS load folding fires. */
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
