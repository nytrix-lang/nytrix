/*
 * Merge induction variables that advance in lockstep.
 *
 * Strength reduction gives every address stream of a loop its own induction
 * variable: `buf[i]`, `buf[i + 1]`, `buf[i + 2]` become three PHIs stepping
 * by the same constant. For two header PHIs
 *
 *   p1 = phi(preheader: i1, latch: p1 + c)
 *   p2 = phi(preheader: i2, latch: p2 + c)
 *
 * p2 == p1 + (i2 - i1) at every point the loop can observe, exactly, under
 * wrapping i64 arithmetic. Each use of p2 is rewritten to p1:
 *
 *   - `B + p2` with B loop-invariant becomes `B' + p1`, with B' = B + D
 *     computed once in the preheader (a new address base, no per-iteration
 *     work);
 *   - any other use reads t = p1 + D, defined after the header PHIs. When D
 *     is a constant the encoder folds it into the addressing displacement.
 *
 * p2 and its latch increment then die. That removes one register and one add
 * per merged stream per iteration (cmov-sort, revcomp lane loops).
 *
 * Legality: one outside predecessor (a preheader) and one latch; every PHI
 * involved has exactly those two incomings; the increment is ADD_I64 by a
 * constant and feeds only its own PHI. Each loop is rewritten on a clone and
 * kept only if the verifier accepts it. NYTRIX_NO_IV_MERGE=1 disables it.
 */
#include "base/debug.h"
#include "code/backend/passes/util.h"
#include <stdlib.h>
#include <string.h>

enum { IVM_MAX_ROUNDS = 16, IVM_MAX_PHIS = 32, IVM_MAX_BASES = 32 };

static bool ivm_scaled_enabled(void) {
  static int disabled = -1;
  if (disabled < 0) {
    const char *e = getenv("NYTRIX_NO_IV_MERGE_SCALED");
    disabled = (e && e[0] && e[0] != '0') ? 1 : 0;
  }
  return !disabled;
}

typedef struct {
  size_t at; /* phi instruction index */
  int phi;
  int init;
  int next;
  size_t next_at;
  int64_t step;
} ivm_iv_t;

typedef struct {
  size_t at;
  nyr_inst_t inst;
} ivm_insert_t;

static nyr_inst_t ivm_make(nyr_op_t op, int dst, int a, int b) {
  nyr_inst_t in = {0};
  in.op = op;
  in.dst = dst;
  in.a = a;
  in.b = b;
  in.c = in.d = in.e = in.f = -1;
  in.cmp = NYIR_CMP_EQ;
  in.effects = nyr_inst_effects(&in);
  return in;
}

static bool ivm_const(const nyr_func_t *f, const int *defs, int defs_count, int v,
                      int64_t *out) {
  for (int depth = 0; depth < 16 && v >= 0 && v < defs_count; ++depth) {
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

static size_t ivm_block_head(const nyr_func_t *f, const nyr_cfg_t *cfg,
                             size_t block) {
  size_t at = cfg->block_start[block];
  size_t end = cfg->block_end[block];
  while (at < end && (f->data[at].op == NYIR_LABEL ||
                      f->data[at].op == NYIR_PHI || f->data[at].op == NYIR_NOP))
    ++at;
  return at;
}

static size_t ivm_block_tail(const nyr_func_t *f, const nyr_cfg_t *cfg,
                             size_t block) {
  size_t end = nyr_block_last_non_nop(f, cfg, block);
  if (end > cfg->block_start[block]) {
    nyr_op_t op = f->data[end - 1].op;
    if (op == NYIR_BR || op == NYIR_BR_IF || op == NYIR_RET)
      return end - 1;
  }
  return end;
}

/*
 * Number of operand uses of v across the function (PHI incomings included).
 */
static size_t ivm_use_count(const nyr_func_t *f, int v) {
  size_t n = 0;
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->op == NYIR_NOP)
      continue;
    const int ops[6] = {in->a, in->b, in->c, in->d, in->e, in->f};
    for (int s = 0; s < 6; ++s)
      n += ops[s] == v;
    for (size_t k = 0; k < in->extra_args_len; ++k)
      n += in->extra_args[k] == v;
    for (size_t k = 0; k < in->phi_incoming_len; ++k)
      n += in->phi_incoming[k].value == v;
  }
  return n;
}

static bool ivm_invariant(const nyr_cfg_t *cfg, const int *defs,
                          const bool *member, int defs_count, int v) {
  /* Staged insertions have fresh SSA IDs, but defs describes the old CFG. */
  if (v < 0 || v >= defs_count)
    return false;
  if (defs[v] < 0)
    return true; /* parameter / entry value */
  return !member[cfg->inst_block[(size_t)defs[v]]];
}

/*
 * Every use of p2 (other than its own PHI and increment) is `B + p2` with a
 * loop-invariant B inside the loop.
 */
static bool ivm_all_rebaseable(const nyr_func_t *f, const nyr_cfg_t *cfg,
                               const int *defs, const bool *member, int defs_count,
                               const ivm_iv_t *p2) {
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->op == NYIR_NOP || i == p2->at || i == p2->next_at)
      continue;
    bool uses = in->c == p2->phi || in->d == p2->phi || in->e == p2->phi ||
                in->f == p2->phi;
    for (size_t k = 0; !uses && k < in->extra_args_len; ++k)
      uses = in->extra_args[k] == p2->phi;
    for (size_t k = 0; !uses && k < in->phi_incoming_len; ++k)
      uses = in->phi_incoming[k].value == p2->phi;
    if (uses)
      return false;
    if (in->a != p2->phi && in->b != p2->phi)
      continue;
    if (in->op != NYIR_ADD_I64 || in->a == in->b || in->extra_args_len ||
        !member[cfg->inst_block[i]])
      return false;
    int other = in->a == p2->phi ? in->b : in->a;
    if (!ivm_invariant(cfg, defs, member, defs_count, other))
      return false;
  }
  return true;
}

/*
 * Decompose `v = scale * root + offset` (mod 2^64) through small integer
 * arithmetic chains so scaled loop inits like `(i << 3)` vs `i - 1` resolve to
 * a constant offset instead of per-outer-iteration materialization.
 */
static bool ivm_linear_of(const nyr_func_t *f, const int *defs, int defs_count, int v,
                          int *root_out, int64_t *scale_out,
                          int64_t *off_out) {
  int cur = v;
  int64_t scale = 1;
  int64_t off = 0;
  for (int depth = 0; depth < 8 && cur >= 0 && cur < defs_count; ++depth) {
    int at = defs[cur];
    if (at < 0)
      break;
    const nyr_inst_t *d = &f->data[at];
    int64_t c = 0;
    if (d->op == NYIR_COPY && d->a >= 0) {
      cur = d->a;
      continue;
    }
    if (d->op == NYIR_ADD_I64 && d->a >= 0 && d->b >= 0) {
      if (ivm_const(f, defs, defs_count, d->b, &c)) {
        off = (int64_t)((uint64_t)off + (uint64_t)scale * (uint64_t)c);
        cur = d->a;
        continue;
      }
      if (ivm_const(f, defs, defs_count, d->a, &c)) {
        off = (int64_t)((uint64_t)off + (uint64_t)scale * (uint64_t)c);
        cur = d->b;
        continue;
      }
    } else if (d->op == NYIR_SUB_I64 && d->a >= 0 && d->b >= 0 &&
               ivm_const(f, defs, defs_count, d->b, &c)) {
      off = (int64_t)((uint64_t)off - (uint64_t)scale * (uint64_t)c);
      cur = d->a;
      continue;
    } else if (d->op == NYIR_SHL_I64 && d->a >= 0 &&
               ivm_const(f, defs, defs_count, d->b, &c) && c >= 0 && c <= 62) {
      scale = (int64_t)((uint64_t)scale * (UINT64_C(1) << (unsigned)c));
      cur = d->a;
      continue;
    } else if (d->op == NYIR_MUL_I64 && d->a >= 0 && d->b >= 0) {
      if (ivm_const(f, defs, defs_count, d->b, &c)) {
        scale = (int64_t)((uint64_t)scale * (uint64_t)c);
        cur = d->a;
        continue;
      }
      if (ivm_const(f, defs, defs_count, d->a, &c)) {
        scale = (int64_t)((uint64_t)scale * (uint64_t)c);
        cur = d->b;
        continue;
      }
    }
    break;
  }
  *root_out = cur;
  *scale_out = scale;
  *off_out = off;
  return cur >= 0 && cur < defs_count;
}

static bool ivm_init_diff(const nyr_func_t *f, const int *defs, int defs_count, int init1,
                          int64_t scale, int init2, int64_t *diff_out) {
  int64_t c1 = 0, c2 = 0;
  if (ivm_const(f, defs, defs_count, init1, &c1) && ivm_const(f, defs, defs_count, init2, &c2)) {
    *diff_out = (int64_t)((uint64_t)c2 - (uint64_t)scale * (uint64_t)c1);
    return true;
  }
  int r1 = -1, r2 = -1;
  int64_t s1 = 0, s2 = 0, o1 = 0, o2 = 0;
  if (ivm_linear_of(f, defs, defs_count, init1, &r1, &s1, &o1) &&
      ivm_linear_of(f, defs, defs_count, init2, &r2, &s2, &o2) && r1 == r2 &&
      (uint64_t)s2 == (uint64_t)scale * (uint64_t)s1) {
    *diff_out = (int64_t)((uint64_t)o2 - (uint64_t)scale * (uint64_t)o1);
    return true;
  }
  return false;
}

/*
 * Collect the merge-eligible header PHIs of one loop.
 */
static size_t ivm_collect(const nyr_func_t *f, const nyr_cfg_t *cfg,
                          const int *defs, const bool *member, size_t header,
                          int64_t pre_label, int64_t latch_label,
                          ivm_iv_t *out) {
  size_t n = 0;
  int defs_count=f->next_value;
  bool allow_sub = ivm_scaled_enabled();
  for (size_t i = cfg->block_start[header];
       i < cfg->block_end[header] && n < IVM_MAX_PHIS; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->op == NYIR_LABEL || in->op == NYIR_NOP)
      continue;
    if (in->op != NYIR_PHI)
      break;
    if (in->dst < 0 || in->phi_incoming_len != 2)
      continue;
    int init = -1, next = -1;
    for (size_t k = 0; k < 2; ++k) {
      if (in->phi_incoming[k].predecessor_label == pre_label)
        init = in->phi_incoming[k].value;
      else if (in->phi_incoming[k].predecessor_label == latch_label)
        next = in->phi_incoming[k].value;
    }
    if (init < 0 || next < 0 || next >= f->next_value || defs[next] < 0)
      continue;
    const nyr_inst_t *inc = &f->data[defs[next]];
    if ((inc->op != NYIR_ADD_I64 &&
         !(allow_sub && inc->op == NYIR_SUB_I64)) ||
        !member[cfg->inst_block[defs[next]]] || inc->extra_args_len)
      continue;
    int64_t step = 0;
    if (inc->op == NYIR_ADD_I64) {
      int other = inc->a == in->dst ? inc->b : inc->b == in->dst ? inc->a : -2;
      if (other == -2 || other == in->dst || !ivm_const(f, defs, defs_count, other, &step))
        continue;
    } else {
      if (inc->a != in->dst || inc->b == in->dst ||
          !ivm_const(f, defs, defs_count, inc->b, &step) || step == INT64_MIN)
        continue;
      step = -step;
    }
    out[n++] = (ivm_iv_t){.at = i,
                          .phi = in->dst,
                          .init = init,
                          .next = next,
                          .next_at = (size_t)defs[next],
                          .step = step};
  }
  return n;
}

/*
 * Rewrite one loop on `f` (a clone). Returns 1 when something merged.
 */
static int ivm_rewrite_loop(nyr_func_t *f, const nyr_cfg_t *cfg,
                            const int *defs, const bool *member, size_t header,
                            size_t pre, int64_t pre_label,
                            int64_t latch_label) {
  const int defs_count=f->next_value;
  ivm_iv_t ivs[IVM_MAX_PHIS];
  size_t n = ivm_collect(f, cfg, defs, member, header, pre_label, latch_label,
                         ivs);
  if (n < 2)
    return 0;
  bool allow_scaled = ivm_scaled_enabled();
  bool merged_any = false;
  bool dead[IVM_MAX_PHIS] = {false};
  ivm_insert_t ins[IVM_MAX_PHIS * (IVM_MAX_BASES * 3 + 8)];
  const size_t ins_cap = sizeof(ins) / sizeof(ins[0]);
  size_t nins = 0;
  size_t pre_at = ivm_block_tail(f, cfg, pre);
  size_t head_at = ivm_block_head(f, cfg, header);
  for (size_t a = 0; a < n; ++a) {
    if (dead[a])
      continue;
    for (size_t b = 0; b < n; ++b) {
      if (a == b || dead[b])
        continue;
      int shift = -1;
      int64_t scale = 1;
      if (ivs[b].step == ivs[a].step) {
        if (b < a)
          continue;
        shift = 0;
        scale = 1;
      } else if (allow_scaled && ivs[a].step != 0) {
        for (int s = 1; s <= 3; ++s) {
          int64_t cand_scale = INT64_C(1) << s;
          if ((ny_i128)ivs[a].step * cand_scale == (ny_i128)ivs[b].step) {
            shift = s;
            scale = cand_scale;
            break;
          }
        }
      }
      if (shift < 0)
        continue;
      const ivm_iv_t *p1 = &ivs[a], *p2 = &ivs[b];
      /*
       * The increment of p2 must feed only p2.
       */
      if (ivm_use_count(f, p2->next) != 1)
        continue;
      if (nins + IVM_MAX_BASES * 3 + 8 > ins_cap)
        break;
      int64_t c1 = 0, const_diff = 0;
      bool k1 = ivm_const(f, defs, defs_count, p1->init, &c1);
      bool has_const_diff =
          ivm_init_diff(f, defs, defs_count, p1->init, scale, p2->init, &const_diff);
      bool zero_diff = (shift == 0 && p1->init == p2->init) ||
                       (has_const_diff && const_diff == 0);
      size_t saved_nins = nins;
      int dval = -1;
      if (zero_diff) {
        /*
         * p2 == p1 << shift; no preheader offset difference to materialize.
         */
      } else if (has_const_diff) {
        dval = f->next_value++;
        nyr_inst_t c = ivm_make(NYIR_CONST_I64, dval, -1, -1);
        c.imm = const_diff;
        ins[nins++] = (ivm_insert_t){pre_at, c};
      } else if (k1 && c1 == 0) {
        dval = p2->init;
      } else if (shift == 0) {
        dval = f->next_value++;
        ins[nins++] = (ivm_insert_t){
            pre_at, ivm_make(NYIR_SUB_I64, dval, p2->init, p1->init)};
      } else {
        int kc = f->next_value++;
        int sc = f->next_value++;
        dval = f->next_value++;
        nyr_inst_t kci = ivm_make(NYIR_CONST_I64, kc, -1, -1);
        kci.imm = shift;
        ins[nins++] = (ivm_insert_t){pre_at, kci};
        ins[nins++] =
            (ivm_insert_t){pre_at, ivm_make(NYIR_SHL_I64, sc, p1->init, kc)};
        ins[nins++] =
            (ivm_insert_t){pre_at, ivm_make(NYIR_SUB_I64, dval, p2->init, sc)};
      }
      /*
       * A scaled IV with nonzero difference (or any non-constant difference)
       * is only profitable when every use rebases onto an invariant address
       * base in the preheader so the loop body folds into x86 SIB [B'+i*s].
       */
      if (!zero_diff && (shift > 0 || !has_const_diff) &&
          !ivm_all_rebaseable(f, cfg, defs, member, defs_count, p2)) {
        nins = saved_nins;
        continue;
      }
      int tval = -1;
      int base_of[IVM_MAX_BASES], base_new[IVM_MAX_BASES];
      size_t nbase = 0;
      bool ok = true;
      for (size_t i = 0; i < f->len && ok; ++i) {
        nyr_inst_t *in = &f->data[i];
        if (in->op == NYIR_NOP || i == p2->at || i == p2->next_at)
          continue;
        bool uses = in->a == p2->phi || in->b == p2->phi ||
                    in->c == p2->phi || in->d == p2->phi ||
                    in->e == p2->phi || in->f == p2->phi;
        for (size_t k = 0; !uses && k < in->extra_args_len; ++k)
          uses = in->extra_args[k] == p2->phi;
        for (size_t k = 0; !uses && k < in->phi_incoming_len; ++k)
          uses = in->phi_incoming[k].value == p2->phi;
        if (!uses)
          continue;
        /*
         * `B + p2` with an invariant B: rebase onto (p1 << shift).
         */
        if ((shift > 0 || !zero_diff) && in->op == NYIR_ADD_I64 &&
            in->a != in->b && !in->extra_args_len &&
            member[cfg->inst_block[i]]) {
          int other = in->a == p2->phi ? in->b : in->a;
          if (ivm_invariant(cfg, defs, member, defs_count, other)) {
            int new_base = other;
            if (!zero_diff) {
              size_t k = 0;
              while (k < nbase && base_of[k] != other)
                ++k;
              if (k == nbase) {
                if (nbase == IVM_MAX_BASES || nins + 1 > ins_cap) {
                  ok = false;
                  break;
                }
                base_of[k] = other;
                base_new[k] = f->next_value++;
                nyr_inst_t add =
                    ivm_make(NYIR_ADD_I64, base_new[k], other, dval);
                add.scalar_class = in->scalar_class;
                ins[nins++] = (ivm_insert_t){pre_at, add};
                ++nbase;
              }
              new_base = base_new[k];
            }
            int repl_idx = p1->phi;
            if (shift > 0) {
              if (nins + 2 > ins_cap) {
                ok = false;
                break;
              }
              int kc = f->next_value++;
              repl_idx = f->next_value++;
              nyr_inst_t kci = ivm_make(NYIR_CONST_I64, kc, -1, -1);
              kci.imm = shift;
              /*
               * Emit const+shl immediately before `in` for x86 SIB folding.
               */
              ins[nins++] = (ivm_insert_t){i, kci};
              ins[nins++] = (ivm_insert_t){
                  i, ivm_make(NYIR_SHL_I64, repl_idx, p1->phi, kc)};
            }
            if (in->a == p2->phi) {
              in->a = repl_idx;
              in->b = new_base;
            } else {
              in->b = repl_idx;
              in->a = new_base;
            }
            nyr_clear_derived_facts(in);
            continue;
          }
        }
        if (zero_diff && shift == 0) {
          tval = p1->phi;
        } else if (tval < 0) {
          if (shift > 0) {
            if (nins + 2 > ins_cap) {
              ok = false;
              break;
            }
            int kc = f->next_value++;
            tval = f->next_value++;
            nyr_inst_t kci = ivm_make(NYIR_CONST_I64, kc, -1, -1);
            kci.imm = shift;
            ins[nins++] = (ivm_insert_t){head_at, kci};
            nyr_inst_t sh = ivm_make(NYIR_SHL_I64, tval, p1->phi, kc);
            sh.scalar_class = f->data[p2->at].scalar_class;
            ins[nins++] = (ivm_insert_t){head_at, sh};
          } else {
            if (nins + 1 > ins_cap) {
              ok = false;
              break;
            }
            tval = f->next_value++;
            nyr_inst_t add = ivm_make(NYIR_ADD_I64, tval, p1->phi, dval);
            add.scalar_class = f->data[p2->at].scalar_class;
            ins[nins++] = (ivm_insert_t){head_at, add};
          }
        }
        int *slots[6] = {&in->a, &in->b, &in->c, &in->d, &in->e, &in->f};
        for (int s = 0; s < 6; ++s)
          if (*slots[s] == p2->phi)
            *slots[s] = tval;
        for (size_t k = 0; k < in->extra_args_len; ++k)
          if (in->extra_args[k] == p2->phi)
            in->extra_args[k] = tval;
        for (size_t k = 0; k < in->phi_incoming_len; ++k)
          if (in->phi_incoming[k].value == p2->phi)
            in->phi_incoming[k].value = tval;
        nyr_clear_derived_facts(in);
      }
      if (!ok)
        return -1;
      /*
       * p2 and its increment are dead now.
       */
      nyr_inst_discard(&f->data[p2->at]);
      nyr_inst_discard(&f->data[p2->next_at]);
      dead[b] = true;
      merged_any = true;
    }
  }
  if (!merged_any)
    return 0;
  /*
   * Insert back to front; same-position groups keep program order.
   */
  for (size_t i = 0; i < nins / 2; ++i) {
    ivm_insert_t t = ins[i];
    ins[i] = ins[nins - 1 - i];
    ins[nins - 1 - i] = t;
  }
  for (size_t i = 1; i < nins; ++i) {
    ivm_insert_t t = ins[i];
    size_t j = i;
    while (j > 0 && ins[j - 1].at < t.at) {
      ins[j] = ins[j - 1];
      --j;
    }
    ins[j] = t;
  }
  if (!nir_ensure_inst_space(f, nins))
    return -1;
  for (size_t i = 0; i < nins; ++i) {
    size_t at = ins[i].at;
    memmove(&f->data[at + 1], &f->data[at], (f->len - at) * sizeof(*f->data));
    f->data[at] = ins[i].inst;
    ++f->len;
  }
  return 1;
}

/*
 * Find one loop that merges, rewrite it on a clone, keep it if it verifies.
 * Returns 1 on a kept rewrite, 0 when no loop changed, -1 on failure.
 */
static int ivm_round(nyr_func_t *f, size_t *skip) {
  nyr_cfg_t cfg = {0};
  if (!nyr_cfg_build(f, &cfg))
    return 0;
  int *defs = nyr_build_defs(f);
  bool *member = calloc(cfg.block_count ? cfg.block_count : 1, sizeof(*member));
  int result = 0;
  size_t seen = 0;
  for (size_t latch = 0; defs && member && latch < cfg.block_count && !result;
       ++latch) {
    if (!cfg.reachable[latch])
      continue;
    for (size_t e = cfg.succ_offsets[latch];
         e < cfg.succ_offsets[latch + 1] && !result; ++e) {
      if (!cfg.backedge_edges[e])
        continue;
      size_t header = cfg.succ_blocks[e];
      if (!nyr_cfg_natural_loop_blocks(&cfg, latch, header, member,
                                       cfg.block_count))
        continue;
      if (seen++ < *skip)
        continue;
      /*
       * Exactly one outside predecessor and this single latch.
       */
      size_t pre = SIZE_MAX, preds = 0;
      bool single_latch = true;
      for (size_t k = cfg.pred_offsets[header]; k < cfg.pred_offsets[header + 1];
           ++k) {
        size_t p = cfg.pred_blocks[k];
        ++preds;
        if (member[p]) {
          if (p != latch)
            single_latch = false;
        } else {
          pre = p;
        }
      }
      if (preds != 2 || pre == SIZE_MAX || !single_latch) {
        ++*skip;
        continue;
      }
      nyr_func_t cand = {0};
      if (!nyr_ir_func_clone(f, &cand)) {
        result = -1;
        break;
      }
      int r = ivm_rewrite_loop(&cand, &cfg, defs, member, header, pre,
                               cfg.block_label[pre], cfg.block_label[latch]);
      char err[256] = {0};
      if (r == 1 && nyr_ir_verify(&cand, err, sizeof(err))) {
        if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
          ny_dbg_heuristic_event("IV_MERGE", "header=L%lld merged",
                                 (long long)cfg.block_label[header]);
        nyr_func_replace(f, &cand);
        result = 1;
      } else {
        if (r == 1 && ny_dbg_is_enabled(NY_DBG_HEURISTIC))
          ny_dbg_heuristic_event("IV_MERGE", "header=L%lld rejected: %s",
                                 (long long)cfg.block_label[header], err);
        nyr_ir_func_free(&cand);
        ++*skip;
      }
    }
  }
  free(member);
  free(defs);
  nyr_cfg_free(&cfg);
  return result;
}

bool nyr_iv_merge(nyr_func_t *f) {
  static int disabled = -1;
  if (disabled < 0) {
    const char *env = getenv("NYTRIX_NO_IV_MERGE");
    disabled = env && *env && strcmp(env, "0") != 0;
  }
  if (disabled || !f || !f->len || f->next_value <= 0 || !nyr_has_loop(f))
    return true;
  size_t skip = 0;
  for (int round = 0; round < IVM_MAX_ROUNDS; ++round) {
    int r = ivm_round(f, &skip);
    if (r < 0)
      return false;
    if (r == 0)
      break;
  }
  return true;
}
