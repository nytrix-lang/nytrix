/*
 * Split a loop test `c1 && c2` back into two branches.
 *
 * Pure `while a < n && b <= m` conditions reach the encoder as
 *
 *   x = and c1 c2 ; br.if x -> body ; br -> exit
 *
 * which x86 materializes as setcc, setcc, movzx, and, test, jcc (mandel's
 * inner loop). Loop tests are predictable, so two compare-and-branch pairs
 * are cheaper:
 *
 *   x' = !c1 ; br.if x' -> exit ; br.if c2 -> body ; br -> exit
 *
 * The inverse compare reuses c1's operands, and both compares stay where
 * they were, so no value moves. Applied only in loop headers whose AND feeds
 * nothing but the branch, with body and exit blocks free of PHIs (the new
 * edges come from label-less blocks). NYTRIX_NO_AND_BRANCH_SPLIT=1 disables
 * it.
 */
#include "code/backend/passes/util.h"
#include <stdlib.h>
#include <string.h>

static nyr_cmp_t abs_invert(nyr_cmp_t c) {
  switch (c) {
  case NYIR_CMP_EQ:
    return NYIR_CMP_NE;
  case NYIR_CMP_NE:
    return NYIR_CMP_EQ;
  case NYIR_CMP_LT:
    return NYIR_CMP_GE;
  case NYIR_CMP_GE:
    return NYIR_CMP_LT;
  case NYIR_CMP_LE:
    return NYIR_CMP_GT;
  case NYIR_CMP_GT:
    return NYIR_CMP_LE;
  }
  return c;
}

static bool abs_is_raw_cmp(const nyr_func_t *f, const int *defs, int v) {
  if (v < 0 || v >= f->next_value || defs[v] < 0)
    return false;
  const nyr_inst_t *d = &f->data[defs[v]];
  return d->op == NYIR_CMP_I64 && d->scalar_class == NYIR_SCALAR_I64_RAW &&
         d->a >= 0 && d->b >= 0 && !(d->flags & NYIR_INST_F_ASSUME);
}

/*
 * A 0/1 test value: a raw compare, or `cmp ^ 1` (if-conversion's form of
 * `!cmp`). Returns the compare and whether it is negated.
 */
static const nyr_inst_t *abs_test(const nyr_func_t *f, const int *defs, int v,
                                  bool *negated) {
  *negated = false;
  if (abs_is_raw_cmp(f, defs, v))
    return &f->data[defs[v]];
  if (v < 0 || v >= f->next_value || defs[v] < 0)
    return NULL;
  const nyr_inst_t *x = &f->data[defs[v]];
  if (x->op != NYIR_XOR_I64 || x->a < 0 || x->b < 0)
    return NULL;
  for (int side = 0; side < 2; ++side) {
    int c = side ? x->a : x->b, other = side ? x->b : x->a;
    if (c < 0 || defs[c] < 0 || f->data[defs[c]].op != NYIR_CONST_I64 ||
        f->data[defs[c]].imm != 1 || !abs_is_raw_cmp(f, defs, other))
      continue;
    *negated = true;
    return &f->data[defs[other]];
  }
  return NULL;
}

static size_t abs_uses(const nyr_func_t *f, int v) {
  size_t n = 0;
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->op == NYIR_NOP)
      continue;
    n += (in->a == v) + (in->b == v) + (in->c == v) + (in->d == v) +
         (in->e == v) + (in->f == v);
    for (size_t k = 0; k < in->extra_args_len; ++k)
      n += in->extra_args[k] == v;
    for (size_t k = 0; k < in->phi_incoming_len; ++k)
      n += in->phi_incoming[k].value == v;
  }
  return n;
}

static bool abs_sink_disabled(void) {
  static int disabled = -1;
  if (disabled < 0) {
    const char *env = getenv("NYTRIX_NO_AND_BRANCH_SPLIT_SINK");
    disabled = (env && *env && strcmp(env, "0") != 0) ? 1 : 0;
  }
  return disabled != 0;
}

static bool abs_chain_disabled(void) {
  static int disabled = -1;
  if (disabled < 0) {
    const char *env = getenv("NYTRIX_NO_AND_BRANCH_SPLIT_CHAIN");
    disabled = (env && *env && strcmp(env, "0") != 0) ? 1 : 0;
  }
  return disabled != 0;
}

typedef struct {
  int val;
  const nyr_inst_t *cmp;
  bool negated;
} abs_chain_leaf_t;

static nyr_inst_t abs_make_cmp(const nyr_inst_t *src, int dst, nyr_cmp_t cmp) {
  nyr_inst_t in = *src;
  in.op = NYIR_CMP_I64;
  in.dst = dst;
  in.cmp = cmp;
  in.c = in.d = in.e = in.f = -1;
  in.extra_args = NULL;
  in.extra_args_len = 0;
  in.arg_sizes = NULL;
  in.phi_incoming = NULL;
  in.phi_incoming_len = 0;
  nyr_clear_derived_facts(&in);
  in.effects = nyr_inst_effects(&in);
  return in;
}

static nyr_inst_t abs_make_br_if(const nyr_inst_t *br_tmpl, int cond,
                                 int64_t label) {
  nyr_inst_t br = *br_tmpl;
  br.op = NYIR_BR_IF;
  br.dst = -1;
  br.a = cond;
  br.b = br.c = br.d = br.e = br.f = -1;
  br.imm = label;
  br.extra_args = NULL;
  br.extra_args_len = 0;
  br.arg_sizes = NULL;
  br.phi_incoming = NULL;
  br.phi_incoming_len = 0;
  nyr_clear_derived_facts(&br);
  br.effects = nyr_inst_effects(&br);
  return br;
}

static bool abs_collect_and_chain(const nyr_func_t *f, const nyr_cfg_t *cfg,
                                  const int *defs, size_t block, int v,
                                  abs_chain_leaf_t leaves[4],
                                  size_t *leaf_count, size_t and_pcs[4],
                                  size_t *and_count, unsigned depth) {
  if (depth > 4 || v < 0 || v >= f->next_value || defs[v] < 0)
    return false;
  size_t def_pc = (size_t)defs[v];
  if (cfg->inst_block[def_pc] != block)
    return false;
  bool neg = false;
  const nyr_inst_t *t = abs_test(f, defs, v, &neg);
  if (t) {
    if (*leaf_count >= 4)
      return false;
    for (size_t k = 0; k < *leaf_count; ++k)
      if (leaves[k].val == v)
        return false;
    leaves[(*leaf_count)++] =
        (abs_chain_leaf_t){.val = v, .cmp = t, .negated = neg};
    return true;
  }
  const nyr_inst_t *in = &f->data[def_pc];
  if (in->op != NYIR_AND_I64 || abs_uses(f, v) != 1 || *and_count >= 4)
    return false;
  and_pcs[(*and_count)++] = def_pc;
  return abs_collect_and_chain(f, cfg, defs, block, in->a, leaves, leaf_count,
                               and_pcs, and_count, depth + 1) &&
         abs_collect_and_chain(f, cfg, defs, block, in->b, leaves, leaf_count,
                               and_pcs, and_count, depth + 1);
}

static void abs_discard_leaf_if_dead(nyr_func_t *cand, const nyr_func_t *orig,
                                     const nyr_cfg_t *cfg, const int *defs,
                                     size_t block, const abs_chain_leaf_t *lf) {
  if (abs_uses(orig, lf->val) != 1)
    return;
  int vp = defs[lf->val];
  if (vp < 0 || cfg->inst_block[(size_t)vp] != block)
    return;
  nyr_inst_discard(&cand->data[vp]);
  if (lf->negated && lf->cmp->dst >= 0 && lf->cmp->dst < orig->next_value &&
      defs[lf->cmp->dst] >= 0 &&
      cfg->inst_block[(size_t)defs[lf->cmp->dst]] == block &&
      abs_uses(orig, lf->cmp->dst) == 1) {
    nyr_inst_discard(&cand->data[defs[lf->cmp->dst]]);
  }
}

static bool abs_block_has_phi(const nyr_func_t *f, const nyr_cfg_t *cfg,
                              int64_t label) {
  for (size_t b = 0; b < cfg->block_count; ++b) {
    if (cfg->block_label[b] != label)
      continue;
    for (size_t i = cfg->block_start[b]; i < cfg->block_end[b]; ++i)
      if (f->data[i].op == NYIR_PHI)
        return true;
    return false;
  }
  return true; /* unknown target: be conservative */
}

bool nyr_and_branch_split(nyr_func_t *f) {
  static int disabled = -1;
  if (disabled < 0) {
    const char *env = getenv("NYTRIX_NO_AND_BRANCH_SPLIT");
    disabled = env && *env && strcmp(env, "0") != 0;
  }
  if (disabled || !f || f->len < 4 || f->next_value <= 0 || !nyr_has_loop(f))
    return true;
  for (int round = 0; round < 16; ++round) {
    nyr_cfg_t cfg = {0};
    if (!nyr_cfg_build(f, &cfg))
      return true;
    int *defs = nyr_build_defs(f);
    bool done = false, failed = !defs;
    for (size_t b = 0; !failed && !done && b < cfg.block_count; ++b) {
      if (!cfg.reachable[b] || b + 1 >= cfg.block_count)
        continue;
      bool header = false;
      for (size_t k = cfg.pred_offsets[b]; k < cfg.pred_offsets[b + 1]; ++k) {
        size_t p = cfg.pred_blocks[k];
        for (size_t e = cfg.succ_offsets[p]; e < cfg.succ_offsets[p + 1]; ++e)
          if (cfg.succ_blocks[e] == b && cfg.backedge_edges[e])
            header = true;
      }
      if (!header)
        continue;
      size_t end = nyr_block_last_non_nop(f, &cfg, b);
      if (end <= cfg.block_start[b])
        continue;
      size_t br_at = end - 1;
      nyr_inst_t *br = &f->data[br_at];
      if (br->op != NYIR_BR_IF || br->a < 0 || defs[br->a] < 0)
        continue;
      size_t and_at = (size_t)defs[br->a];
      nyr_inst_t *andi = &f->data[and_at];
      if (andi->op != NYIR_AND_I64 || cfg.inst_block[and_at] != b ||
          abs_uses(f, br->a) != 1)
        continue;
      /*
       * Fall-through: a label-less block holding only `br exit`.
       */
      size_t nb = b + 1;
      size_t nb_end = nyr_block_last_non_nop(f, &cfg, nb);
      if (cfg.block_label[nb] >= 0 || nb_end != cfg.block_start[nb] + 1 ||
          f->data[cfg.block_start[nb]].op != NYIR_BR)
        continue;
      int64_t exit_label = f->data[cfg.block_start[nb]].imm;
      int64_t body_label = br->imm;
      if (exit_label == body_label ||
          abs_block_has_phi(f, &cfg, exit_label) ||
          abs_block_has_phi(f, &cfg, body_label))
        continue;
      bool neg1 = false, neg2 = false;
      const nyr_inst_t *t1 = abs_test(f, defs, andi->a, &neg1);
      const nyr_inst_t *t2 = abs_test(f, defs, andi->b, &neg2);
      if (!t1 || !t2) {
        if (abs_chain_disabled())
          continue;
        abs_chain_leaf_t leaves[4] = {0};
        size_t and_pcs[4] = {0};
        size_t leaf_count = 0, and_count = 0;
        if (!abs_collect_and_chain(f, &cfg, defs, b, br->a, leaves, &leaf_count,
                                   and_pcs, &and_count, 0) ||
            leaf_count < 3 || leaf_count > 4)
          continue;
        nyr_func_t cand = {0};
        if (!nyr_ir_func_clone(f, &cand))
          continue;
        for (size_t k = 0; k < and_count; ++k)
          nyr_inst_discard(&cand.data[and_pcs[k]]);
        nyr_inst_t emitted[10];
        size_t em_len = 0;
        for (size_t m = 0; m + 1 < leaf_count; ++m) {
          abs_discard_leaf_if_dead(&cand, f, &cfg, defs, b, &leaves[m]);
          nyr_cmp_t inv_cmp = leaves[m].negated
                                  ? leaves[m].cmp->cmp
                                  : abs_invert(leaves[m].cmp->cmp);
          int inv_v = cand.next_value++;
          emitted[em_len++] = abs_make_cmp(leaves[m].cmp, inv_v, inv_cmp);
          emitted[em_len++] = abs_make_br_if(br, inv_v, exit_label);
        }
        const abs_chain_leaf_t *last = &leaves[leaf_count - 1];
        int last_cond = last->val;
        if (abs_uses(f, last->val) == 1 &&
            (!last->negated || abs_uses(f, last->cmp->dst) == 1)) {
          abs_discard_leaf_if_dead(&cand, f, &cfg, defs, b, last);
          nyr_cmp_t fwd_cmp =
              last->negated ? abs_invert(last->cmp->cmp) : last->cmp->cmp;
          last_cond = cand.next_value++;
          emitted[em_len++] = abs_make_cmp(last->cmp, last_cond, fwd_cmp);
        }
        emitted[em_len++] = abs_make_br_if(br, last_cond, body_label);
        if (em_len > 1 && nir_ensure_inst_space(&cand, em_len - 1)) {
          nyr_inst_discard(&cand.data[br_at]);
          memmove(&cand.data[br_at + em_len], &cand.data[br_at + 1],
                  (cand.len - (br_at + 1)) * sizeof(*cand.data));
          for (size_t k = 0; k < em_len; ++k)
            cand.data[br_at + k] = emitted[k];
          cand.len += em_len - 1;
          char err[256] = {0};
          if (nyr_ir_verify(&cand, err, sizeof(err))) {
            nyr_func_replace(f, &cand);
            done = true;
            continue;
          }
        }
        nyr_ir_func_free(&cand);
        continue;
      }
      const nyr_inst_t *c1 = t1;
      int c2 = andi->b;
      /*
       * x = !c1, in place of the AND.
       */
      int x = andi->dst;
      nyr_inst_t inv = *c1;
      inv.dst = x;
      inv.cmp = neg1 ? c1->cmp : abs_invert(c1->cmp);
      inv.extra_args = NULL;
      inv.extra_args_len = 0;
      inv.arg_sizes = NULL;
      inv.phi_incoming = NULL;
      inv.phi_incoming_len = 0;
      inv.debug = andi->debug;
      nyr_clear_derived_facts(&inv);
      inv.effects = nyr_inst_effects(&inv);
      nyr_inst_t second = *br;
      second.a = c2;
      second.imm = body_label;
      second.extra_args = NULL;
      second.extra_args_len = 0;
      second.arg_sizes = NULL;
      second.phi_incoming = NULL;
      second.phi_incoming_len = 0;

      nyr_inst_t sunk_insts[4];
      size_t sunk_n = 0;
      bool can_sink = false;
      int xor_at = -1, cmp_at = -1;
      int feed_idx[2] = {-1, -1};
      int feed_n = 0;
      if (!abs_sink_disabled() && c2 >= 0 && c2 < f->next_value &&
          defs[c2] >= 0 && cfg.inst_block[(size_t)defs[c2]] == b &&
          (size_t)defs[c2] != and_at && abs_uses(f, c2) == 1) {
        if (!neg2) {
          cmp_at = defs[c2];
          can_sink = true;
        } else if (t2->dst >= 0 && t2->dst < f->next_value &&
                   defs[t2->dst] >= 0 &&
                   cfg.inst_block[(size_t)defs[t2->dst]] == b &&
                   (size_t)defs[t2->dst] != and_at &&
                   defs[t2->dst] < defs[c2] && abs_uses(f, t2->dst) == 1) {
          cmp_at = defs[t2->dst];
          xor_at = defs[c2];
          can_sink = true;
        }
      }
      if (can_sink) {
        int ops[2] = {t2->a, t2->b};
        for (int oi = 0; oi < 2; ++oi) {
          int op = ops[oi];
          if (oi == 1 && op == ops[0])
            continue;
          if (op < 0 || op >= f->next_value || defs[op] < 0)
            continue;
          int fi = defs[op];
          if (cfg.inst_block[(size_t)fi] != b || fi >= cmp_at ||
              fi == (int)and_at)
            continue;
          if (op == c1->a || op == c1->b || op == x || op == andi->a)
            continue;
          const nyr_inst_t *fd = &f->data[fi];
          if ((fd->op != NYIR_ADD_I64 && fd->op != NYIR_SUB_I64 &&
               fd->op != NYIR_MUL_I64) ||
              fd->extra_args || fd->arg_sizes || fd->phi_incoming ||
              nyr_effective_effects(fd) != NYIR_EFFECT_NONE ||
              abs_uses(f, op) != 1)
            continue;
          feed_idx[feed_n++] = fi;
        }
        if (feed_n == 2 && feed_idx[0] > feed_idx[1]) {
          int tmp = feed_idx[0];
          feed_idx[0] = feed_idx[1];
          feed_idx[1] = tmp;
        }
        for (int k = 0; k < feed_n; ++k) {
          nyr_inst_t fd = f->data[feed_idx[k]];
          fd.extra_args = NULL;
          fd.extra_args_len = 0;
          fd.arg_sizes = NULL;
          fd.phi_incoming = NULL;
          fd.phi_incoming_len = 0;
          nyr_clear_derived_facts(&fd);
          fd.effects = nyr_inst_effects(&fd);
          sunk_insts[sunk_n++] = fd;
        }
        nyr_inst_t sunk_cmp = *t2;
        sunk_cmp.dst = c2;
        sunk_cmp.cmp = neg2 ? abs_invert(t2->cmp) : t2->cmp;
        sunk_cmp.extra_args = NULL;
        sunk_cmp.extra_args_len = 0;
        sunk_cmp.arg_sizes = NULL;
        sunk_cmp.phi_incoming = NULL;
        sunk_cmp.phi_incoming_len = 0;
        nyr_clear_derived_facts(&sunk_cmp);
        sunk_cmp.effects = nyr_inst_effects(&sunk_cmp);
        sunk_insts[sunk_n++] = sunk_cmp;
      }

      if (can_sink && sunk_n > 0) {
        nyr_func_t cand = {0};
        if (nyr_ir_func_clone(f, &cand)) {
          nyr_inst_t nop = {.op = NYIR_NOP,
                            .dst = -1,
                            .a = -1,
                            .b = -1,
                            .c = -1,
                            .d = -1,
                            .e = -1,
                            .f = -1};
          nyr_inst_discard(&cand.data[and_at]);
          cand.data[and_at] = inv;
          cand.data[br_at].imm = exit_label;
          nyr_inst_discard(&cand.data[cmp_at]);
          cand.data[cmp_at] = nop;
          if (xor_at >= 0) {
            nyr_inst_discard(&cand.data[xor_at]);
            cand.data[xor_at] = nop;
          }
          for (int k = 0; k < feed_n; ++k) {
            nyr_inst_discard(&cand.data[feed_idx[k]]);
            cand.data[feed_idx[k]] = nop;
          }
          if (nir_ensure_inst_space(&cand, sunk_n + 1)) {
            size_t at = br_at + 1;
            memmove(&cand.data[at + sunk_n + 1], &cand.data[at],
                    (cand.len - at) * sizeof(*cand.data));
            for (size_t k = 0; k < sunk_n; ++k)
              cand.data[at + k] = sunk_insts[k];
            cand.data[at + sunk_n] = second;
            cand.len += sunk_n + 1;
            char err[256] = {0};
            if (nyr_ir_verify(&cand, err, sizeof(err))) {
              nyr_func_replace(f, &cand);
              done = true;
              continue;
            }
          }
          nyr_ir_func_free(&cand);
        }
      }

      *andi = inv;
      br->imm = exit_label;
      if (!nir_ensure_inst_space(f, 1)) {
        failed = true;
        break;
      }
      size_t at = br_at + 1;
      memmove(&f->data[at + 1], &f->data[at], (f->len - at) * sizeof(*f->data));
      f->data[at] = second;
      ++f->len;
      done = true;
    }
    free(defs);
    nyr_cfg_free(&cfg);
    if (failed)
      return false;
    if (!done)
      break;
  }
  return true;
}
