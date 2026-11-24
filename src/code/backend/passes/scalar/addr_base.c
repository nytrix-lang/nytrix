/*
 * Late address reassociation, base side.
 *
 * Unrolled loops address neighbours as (B + C_k) + x: every C_k gives its
 * own loop-invariant base, and each base takes a register (revcomp's
 * unrolled lane loop kept 16 bases and spilled 27 of them). When at least
 * four addresses in one block share (B, x), rewrite each as (B + x) + C_k: the shared
 * B + x is computed once and every access becomes [S + disp] through the
 * encoder's displacement fold. Wrapping i64 arithmetic makes it exact.
 * NYTRIX_NO_ADDR_BASE_REASSOC=1 disables it.
 */
#include "code/backend/passes/util.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
  size_t at;
  int base, x;
  int64_t disp;
} ab_cand_t;

typedef struct {
  size_t at;
  nyr_inst_t inst;
} ab_insert_t;

static bool ab_const(const nyr_func_t *f, const int *defs, int v,
                     int64_t *out) {
  if (v < 0 || v >= f->next_value || defs[v] < 0)
    return false;
  const nyr_inst_t *d = &f->data[defs[v]];
  if (d->op != NYIR_CONST_I64)
    return false;
  *out = d->imm;
  return true;
}

bool nyr_addr_base_reassoc(nyr_func_t *f) {
  static int disabled = -1;
  if (disabled < 0) {
    const char *env = getenv("NYTRIX_NO_ADDR_BASE_REASSOC");
    disabled = env && *env && strcmp(env, "0") != 0;
  }
  if (disabled || !f || f->next_value <= 0 || f->len < 4)
    return true;
  int *defs = nyr_build_defs(f);
  if (!defs)
    return false;
  /* Only addresses: values used solely as load/store address operands. */
  size_t nv = (size_t)f->next_value;
  unsigned char *bad = calloc(nv, 1);
  unsigned char *used = calloc(nv, 1);
  ab_cand_t *cands = NULL;
  ab_insert_t *ins = NULL;
  int *group = NULL;
  nyr_cfg_t cfg = {0};
  bool cfg_ok = false;
  bool ok = bad && used;
  for (size_t i = 0; ok && i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    const int ops[6] = {in->a, in->b, in->c, in->d, in->e, in->f};
    for (int s = 0; s < 6; ++s) {
      int v = ops[s];
      if (v < 0 || (size_t)v >= nv)
        continue;
      used[v] = 1;
      bool addr = s == 0 &&
                  (in->op == NYIR_LOAD_I64 || in->op == NYIR_STORE_I64) &&
                  in->c != v && in->b != v;
      if (!addr)
        bad[v] = 1;
    }
    for (size_t k = 0; k < in->extra_args_len; ++k)
      if (in->extra_args[k] >= 0 && (size_t)in->extra_args[k] < nv)
        bad[in->extra_args[k]] = used[in->extra_args[k]] = 1;
    for (size_t k = 0; k < in->phi_incoming_len; ++k) {
      int v = in->phi_incoming[k].value;
      if (v >= 0 && (size_t)v < nv)
        bad[v] = used[v] = 1;
    }
  }
  size_t n = 0, cap = 0;
  for (size_t i = 0; ok && i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->op != NYIR_ADD_I64 || in->dst < 0 || in->a < 0 || in->b < 0 ||
        (size_t)in->dst >= nv || bad[in->dst] || !used[in->dst] ||
        in->a >= f->next_value || in->b >= f->next_value)
      continue;
    for (int side = 0; side < 2; ++side) {
      int bk = side ? in->b : in->a;
      int x = side ? in->a : in->b;
      int64_t tmp = 0;
      if (ab_const(f, defs, x, &tmp) || defs[bk] < 0)
        continue;
      const nyr_inst_t *bd = &f->data[defs[bk]];
      if (bd->op != NYIR_ADD_I64 || bd->a < 0 || bd->b < 0)
        continue;
      int64_t c = 0;
      int base = -1;
      if (ab_const(f, defs, bd->b, &c) && !ab_const(f, defs, bd->a, &tmp))
        base = bd->a;
      else if (ab_const(f, defs, bd->a, &c) && !ab_const(f, defs, bd->b, &tmp))
        base = bd->b;
      if (base < 0 || base == x || c > INT32_MAX || c < INT32_MIN)
        continue;
      /* Frame bases already address as [rbp + x + slot] in the encoder. */
      if (defs[base] >= 0 && (f->data[defs[base]].op == NYIR_ALLOCA ||
                              f->data[defs[base]].op == NYIR_ADDR_LOCAL))
        continue;
      /*
       * The displacement folds only into the access right after the add;
       * anywhere else the rewrite costs an add.
       */
      const nyr_inst_t *next = i + 1 < f->len ? &f->data[i + 1] : NULL;
      if (!next ||
          (next->op != NYIR_LOAD_I64 && next->op != NYIR_STORE_I64) ||
          next->a != in->dst)
        continue;
      if (n == cap) {
        size_t nc = cap ? cap * 2 : 32;
        ab_cand_t *nd = realloc(cands, nc * sizeof(*nd));
        if (!nd) {
          ok = false;
          break;
        }
        cands = nd, cap = nc;
      }
      cands[n++] = (ab_cand_t){.at = i, .base = base, .x = x, .disp = c};
      break;
    }
  }
  if (ok && n >= 2) {
    group = malloc(n * sizeof(int));
    cfg_ok = group && nyr_cfg_build(f, &cfg);
    ok = group != NULL;
  }
  size_t ins_n = 0, ins_cap = 0;
  for (size_t i = 0; cfg_ok && ok && i < n; ++i) {
    group[i] = (int)i;
    for (size_t j = 0; j < i; ++j)
      if (cands[j].base == cands[i].base && cands[j].x == cands[i].x &&
          cfg.inst_block[cands[j].at] == cfg.inst_block[cands[i].at]) {
        group[i] = group[j];
        break;
      }
  }
  for (size_t g = 0; cfg_ok && ok && g < n; ++g) {
    if (group[g] != (int)g)
      continue;
    size_t members = 0;
    for (size_t i = g; i < n; ++i)
      members += group[i] == (int)g;
    /*
     * Only wide unrolled bodies run out of registers for their bases; a
     * couple of shared accesses gain nothing and perturb allocation
     * (expression-tree +28% cycles at 2-3 members).
     */
    if (members < 4)
      continue;
    const ab_cand_t *c = &cands[g];
    /*
     * S = B + x right before the first member: every member is in this
     * block, and B and x already reach the first one, so S lives only across
     * the block's own accesses.
     */
    size_t p = c->at;
    size_t need = ins_n + 1 + members;
    if (need > ins_cap) {
      size_t nc = ins_cap ? ins_cap : 16;
      while (nc < need)
        nc *= 2;
      ab_insert_t *nd = realloc(ins, nc * sizeof(*nd));
      if (!nd) {
        ok = false;
        break;
      }
      ins = nd, ins_cap = nc;
    }
    const nyr_inst_t *first = &f->data[c->at];
    int sv = f->next_value++;
    nyr_inst_t s = *first;
    s.dst = sv;
    s.a = c->base;
    s.b = c->x;
    s.c = s.d = s.e = s.f = -1;
    s.extra_args = NULL;
    s.extra_args_len = 0;
    s.arg_sizes = NULL;
    s.phi_incoming = NULL;
    s.phi_incoming_len = 0;
    nyr_clear_derived_facts(&s);
    ins[ins_n++] = (ab_insert_t){p, s};
    for (size_t i = g; i < n; ++i) {
      if (group[i] != (int)g)
        continue;
      nyr_inst_t *d = &f->data[cands[i].at];
      nyr_inst_t k = {.op = NYIR_CONST_I64,
                      .dst = f->next_value++,
                      .a = -1, .b = -1, .c = -1, .d = -1, .e = -1, .f = -1,
                      .imm = cands[i].disp,
                      .debug = d->debug};
      k.effects = nyr_inst_effects(&k);
      /* The constant sits right before its user so the fold stays adjacent. */
      ins[ins_n++] = (ab_insert_t){cands[i].at, k};
      d->a = sv;
      d->b = k.dst;
      nyr_clear_derived_facts(d);
    }
  }
  if (ok && ins_n) {
    for (size_t i = 1; i < ins_n; ++i) {
      ab_insert_t t = ins[i];
      size_t j = i;
      while (j > 0 && ins[j - 1].at < t.at) {
        ins[j] = ins[j - 1];
        --j;
      }
      ins[j] = t;
    }
    if (!nir_ensure_inst_space(f, ins_n)) {
      ok = false;
    } else {
      for (size_t i = 0; i < ins_n; ++i) {
        size_t at = ins[i].at;
        memmove(&f->data[at + 1], &f->data[at],
                (f->len - at) * sizeof(*f->data));
        f->data[at] = ins[i].inst;
        ++f->len;
      }
    }
  }
  if (cfg_ok)
    nyr_cfg_free(&cfg);
  free(group);
  free(ins);
  free(cands);
  free(bad);
  free(used);
  free(defs);
  return ok;
}
