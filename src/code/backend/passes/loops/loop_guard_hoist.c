/*
 * Hoist loop-invariant exit guards out of loop headers.
 *
 *   P:  ...                      P:  ...
 *   H:  phis                         br.if c -> T
 *       br.if c -> T                 br -> N
 *       br -> B            ==>   N:  (dedicated preheader)
 *                                H:  phis
 *                                    br -> B
 *
 * The guard condition c is defined outside the loop, so it has the same value
 * on every header entry. The header runs first on loop entry, and only PHIs
 * and copies precede the guard, so the guard either fires on that first entry
 * (before any loop effect) or never. Branching from the preheader instead is
 * exact provided the target T is outside the loop, has no PHIs, and reads no
 * value defined inside the loop. The typical guard is a divide-by-zero or
 * bounds panic on an invariant divisor; removing it leaves the loop with the
 * single exit that loop transforms such as the outer vectorizer require.
 */
#include "code/backend/passes/util.h"
#include <stdlib.h>
#include <string.h>

enum { LGH_MAX_REWRITES = 16 };

static bool lgh_insert(nyr_func_t *f, size_t at, nyr_inst_t in) {
  if (!nir_ensure_inst_space(f, 1))
    return false;
  memmove(&f->data[at + 1], &f->data[at], (f->len - at) * sizeof(*f->data));
  f->data[at] = in;
  f->len++;
  return true;
}

static nyr_inst_t lgh_ctrl(nyr_op_t op, int a, int64_t imm) {
  return (nyr_inst_t){.op = op,
                      .dst = -1,
                      .a = a,
                      .b = -1,
                      .c = -1,
                      .d = -1,
                      .e = -1,
                      .f = -1,
                      .imm = imm,
                      .effects = op == NYIR_LABEL ? NYIR_EFFECT_NONE
                                                  : NYIR_EFFECT_CONTROL};
}

static bool lgh_inst_reads_loop_value(const nyr_inst_t *in, const int *defs,
                                      const nyr_cfg_t *cfg,
                                      const bool *member, int limit) {
  const int ops[] = {in->a, in->b, in->c, in->d, in->e, in->f};
  for (size_t k = 0; k < sizeof(ops) / sizeof(ops[0]); ++k) {
    int v = ops[k];
    if (v >= 0 && v < limit && defs[v] >= 0 &&
        member[cfg->inst_block[defs[v]]])
      return true;
  }
  for (size_t k = 0; k < in->extra_args_len; ++k) {
    int v = in->extra_args[k];
    if (v >= 0 && v < limit && defs[v] >= 0 &&
        member[cfg->inst_block[defs[v]]])
      return true;
  }
  return false;
}

static int lgh_rewrite_once(nyr_func_t *f) {
  nyr_cfg_t cfg = {0};
  if (!nyr_cfg_build(f, &cfg))
    return 0;
  int *defs = nyr_build_defs(f);
  bool *member = calloc(cfg.block_count ? cfg.block_count : 1, sizeof(bool));
  int result = 0;
  if (!defs || !member)
    goto done;
  int64_t max_label = -1;
  for (size_t i = 0; i < f->len; ++i)
    if (f->data[i].op == NYIR_LABEL && f->data[i].imm > max_label)
      max_label = f->data[i].imm;
  for (size_t latch = 0; latch < cfg.block_count && !result; ++latch) {
    for (size_t e = cfg.succ_offsets[latch];
         e < cfg.succ_offsets[latch + 1] && !result; ++e) {
      if (!cfg.backedge_edges[e])
        continue;
      size_t header = cfg.succ_blocks[e];
      memset(member, 0, cfg.block_count * sizeof(bool));
      if (!nyr_cfg_natural_loop_blocks(&cfg, latch, header, member,
                                       cfg.block_count))
        continue;
      size_t pre = SIZE_MAX, outside = 0;
      for (size_t k = cfg.pred_offsets[header];
           k < cfg.pred_offsets[header + 1]; ++k)
        if (!member[cfg.pred_blocks[k]]) {
          pre = cfg.pred_blocks[k];
          ++outside;
        }
      if (outside != 1 || cfg.block_label[header] < 0 ||
          cfg.succ_offsets[pre + 1] - cfg.succ_offsets[pre] != 1)
        continue;
      /*
       * The guard: first non-PHI/COPY instruction of the header.
       */
      size_t gi = cfg.block_start[header];
      while (gi < cfg.block_end[header]) {
        nyr_op_t op = f->data[gi].op;
        if (op == NYIR_LABEL || op == NYIR_NOP || op == NYIR_PHI ||
            op == NYIR_COPY) {
          ++gi;
          continue;
        }
        break;
      }
      if (gi >= cfg.block_end[header] || f->data[gi].op != NYIR_BR_IF)
        continue;
      const nyr_inst_t *g = &f->data[gi];
      int cond = g->a;
      if (cond < 0 || cond >= f->next_value || defs[cond] < 0)
        continue;
      /*
       * A COPY of an outside value inside the header is still invariant.
       */
      int root = cond;
      while (root >= 0 && defs[root] >= 0 &&
             f->data[defs[root]].op == NYIR_COPY &&
             member[cfg.inst_block[defs[root]]])
        root = f->data[defs[root]].a;
      if (root < 0 || defs[root] < 0 || member[cfg.inst_block[defs[root]]])
        continue;
      size_t target = 0;
      if (!nyr_cfg_block_for_label(&cfg, g->imm, &target) || member[target])
        continue;
      bool target_ok = true;
      for (size_t i = cfg.block_start[target];
           i < cfg.block_end[target] && target_ok; ++i) {
        const nyr_inst_t *in = &f->data[i];
        if (in->op == NYIR_PHI ||
            lgh_inst_reads_loop_value(in, defs, &cfg, member, f->next_value))
          target_ok = false;
      }
      if (!target_ok)
        continue;
      /*
       * Insertion point: end of the preheader, before a `br -> H`.
       */
      size_t pre_end = cfg.block_end[pre];
      size_t at = pre_end;
      while (at > cfg.block_start[pre] && f->data[at - 1].op == NYIR_NOP)
        --at;
      if (at > cfg.block_start[pre] && f->data[at - 1].op == NYIR_BR)
        --at;
      else if (at > cfg.block_start[pre] &&
               (f->data[at - 1].op == NYIR_BR_IF ||
                f->data[at - 1].op == NYIR_RET))
        continue;
      if (at > gi)
        continue; /* layout must place the preheader before the header */
      int64_t old_pre_label = cfg.block_label[pre];
      int64_t new_label = max_label + 1;
      int64_t target_label = g->imm;
      /*
       * Header: drop the guard (later instructions shift by three).
       */
      nyr_inst_discard(&f->data[gi]);
      if (!lgh_insert(f, at, lgh_ctrl(NYIR_LABEL, -1, new_label)) ||
          !lgh_insert(f, at, lgh_ctrl(NYIR_BR, -1, new_label)) ||
          !lgh_insert(f, at, lgh_ctrl(NYIR_BR_IF, root, target_label))) {
        result = -1;
        goto done;
      }
      /*
       * Header PHIs now arrive from the new preheader.
       */
      for (size_t i = 0; i < f->len; ++i) {
        nyr_inst_t *in = &f->data[i];
        if (in->op != NYIR_PHI)
          continue;
        for (size_t k = 0; k < in->phi_incoming_len; ++k)
          if (in->phi_incoming[k].predecessor_label == old_pre_label &&
              i > at + 2 && i < cfg.block_end[header] + 3)
            in->phi_incoming[k].predecessor_label = new_label;
      }
      result = 1;
    }
  }
done:
  free(defs);
  free(member);
  nyr_cfg_free(&cfg);
  return result;
}

bool nyr_loop_guard_hoist(nyr_func_t *f) {
  if (!f || !f->len || !nyr_has_loop(f) || getenv("NYTRIX_NO_LOOP_GUARD_HOIST"))
    return true;
  for (int n = 0; n < LGH_MAX_REWRITES; ++n) {
    int r = lgh_rewrite_once(f);
    if (r < 0)
      return false;
    if (r == 0)
      break;
  }
  return true;
}
