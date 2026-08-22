/*
 * Copy propagation: replaces uses of register-copy results with the
 * original source value when the copy chain is provably transparent.
 *
 * Straight-line functions use a linear alias map.  Functions with control
 * flow use a dominator-checked walk: a COPY dst <- src may only be folded
 * into a use when src is available at that use (src's definition dominates
 * the copy, and the copy dominates the use / the predecessor edge of a PHI
 * incoming value).  NYIR values are defined once, so in SSA form these
 * checks always succeed and the pass collapses the `mov` noise that
 * mem2reg and PHI simplification leave between PHIs and their uses.
 */
#include "code/native/ir/opt/util.h"
#include "code/native/ir/opt/loop_analysis.h"
#include "code/native/ir/internal.h"
#include "base/compat.h"
#include "base/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int nir_alias_find(const int *alias, int v) {
  if (!alias || v < 0)
    return v;
  int cur = v;
  for (int depth = 0; depth < 64 && alias[cur] >= 0 && alias[cur] != cur; ++depth)
    cur = alias[cur];
  return cur;
}

/*
 * Dominator-sound copy propagation for functions with control flow.
 *
 * alias[v]  = source value v may be replaced by, or -1.
 * alias_blk[v] = block of the COPY that established alias[v] (SIZE_MAX for
 *                entry values, which dominate every block).
 *
 * A use is only rewritten when the aliased source is provably available:
 *   - instruction operands: the copy's block dominates the use's block, or
 *     the use follows the copy in the same block;
 *   - PHI incoming values: the copy's block is the edge's predecessor or
 *     dominates it (the value flows from that predecessor).
 */
static bool copy_prop_cfg(nyir_func_t *f) {
  int *defs = nyir_build_defs(f);
  nyir_cfg_t cfg = {0};
  if (!defs || !nyir_cfg_build(f, &cfg)) {
    free(defs);
    nyir_cfg_free(&cfg);
    return false;
  }
  size_t nv = (size_t)f->next_value;
  int stk_alias[256];
  size_t stk_alias_blk[256];
  int *alias = nv <= 256 ? stk_alias : (int *)malloc(nv * sizeof(int));
  size_t *alias_blk = nv <= 256 ? stk_alias_blk : (size_t *)malloc(nv * sizeof(size_t));
  if (!alias || !alias_blk) {
    if (nv > 256) { free(alias); free(alias_blk); }
    free(defs);
    nyir_cfg_free(&cfg);
    return false;
  }
  for (int i = 0; i < f->next_value; ++i) {
    alias[i] = -1;
    alias_blk[i] = SIZE_MAX;
  }
  for (size_t i = 0; i < f->len; ++i) {
    nyir_inst_t *in = &f->data[i];
    if (in->dst < 0) {
      /*
       * No definition here: nothing to alias, but operands may still fold.
       */
      if (in->a >= 0) in->a = nir_alias_find(alias, in->a);
      if (in->b >= 0) in->b = nir_alias_find(alias, in->b);
      if (in->c >= 0) in->c = nir_alias_find(alias, in->c);
      if (in->d >= 0) in->d = nir_alias_find(alias, in->d);
      if (in->e >= 0) in->e = nir_alias_find(alias, in->e);
      if (in->f >= 0) in->f = nir_alias_find(alias, in->f);
      continue;
    }
    size_t blk = cfg.inst_block[i];
    /*
     * Fold operands whose aliased source is available here.
     */
    if (in->a >= 0) {
      int s = nir_alias_find(alias, in->a);
      if (s != in->a &&
          (alias_blk[in->a] == SIZE_MAX ||
           nyir_cfg_dominates(&cfg, alias_blk[in->a], blk)))
        in->a = s;
    }
    if (in->b >= 0) {
      int s = nir_alias_find(alias, in->b);
      if (s != in->b &&
          (alias_blk[in->b] == SIZE_MAX ||
           nyir_cfg_dominates(&cfg, alias_blk[in->b], blk)))
        in->b = s;
    }
    if (in->c >= 0) {
      int s = nir_alias_find(alias, in->c);
      if (s != in->c &&
          (alias_blk[in->c] == SIZE_MAX ||
           nyir_cfg_dominates(&cfg, alias_blk[in->c], blk)))
        in->c = s;
    }
    if (in->d >= 0) {
      int s = nir_alias_find(alias, in->d);
      if (s != in->d &&
          (alias_blk[in->d] == SIZE_MAX ||
           nyir_cfg_dominates(&cfg, alias_blk[in->d], blk)))
        in->d = s;
    }
    if (in->e >= 0) {
      int s = nir_alias_find(alias, in->e);
      if (s != in->e &&
          (alias_blk[in->e] == SIZE_MAX ||
           nyir_cfg_dominates(&cfg, alias_blk[in->e], blk)))
        in->e = s;
    }
    if (in->f >= 0) {
      int s = nir_alias_find(alias, in->f);
      if (s != in->f &&
          (alias_blk[in->f] == SIZE_MAX ||
           nyir_cfg_dominates(&cfg, alias_blk[in->f], blk)))
        in->f = s;
    }
    for (size_t k = 0; k < in->extra_args_len; ++k) {
      int v = in->extra_args[k];
      if (v < 0)
        continue;
      int s = nir_alias_find(alias, v);
      if (s != v &&
          (alias_blk[v] == SIZE_MAX ||
           nyir_cfg_dominates(&cfg, alias_blk[v], blk)))
        in->extra_args[k] = s;
    }
    /*
     * PHI incoming values are used on the predecessor edge, so the aliased
     * source must be available at the end of that predecessor block.
     */
    for (size_t k = 0; k < in->phi_incoming_len; ++k) {
      int v = in->phi_incoming[k].value;
      if (v < 0)
        continue;
      int s = nir_alias_find(alias, v);
      if (s == v)
        continue;
      size_t pred_blk = SIZE_MAX;
      for (size_t e = cfg.pred_offsets[blk]; e < cfg.pred_offsets[blk + 1]; ++e) {
        size_t pred = cfg.pred_blocks[e];
        if (cfg.block_label[pred] == in->phi_incoming[k].predecessor_label) {
          pred_blk = pred;
          break;
        }
      }
      if (pred_blk == SIZE_MAX)
        continue;
      if (alias_blk[v] == SIZE_MAX ||
          nyir_cfg_dominates(&cfg, alias_blk[v], pred_blk) ||
          alias_blk[v] == pred_blk)
        in->phi_incoming[k].value = s;
    }

    if (in->op == NYIR_COPY && in->a >= 0) {
      int s = nir_alias_find(alias, in->a);
      bool safe = false;
      size_t src_blk = SIZE_MAX;
      if (defs[s] < 0) {
        safe = true; /* entry value: available everywhere */
      } else {
        src_blk = cfg.inst_block[(size_t)defs[s]];
        safe = nyir_cfg_dominates(&cfg, src_blk, blk);
      }
      if (safe) {
        alias[in->dst] = s;
        alias_blk[in->dst] = src_blk == SIZE_MAX ? SIZE_MAX : blk;
      } else {
        alias[in->dst] = -1;
        alias_blk[in->dst] = SIZE_MAX;
      }
    } else {
      alias[in->dst] = -1;
      alias_blk[in->dst] = SIZE_MAX;
    }
  }
  if (nv > 256) { free(alias); free(alias_blk); }
  free(defs);
  nyir_cfg_free(&cfg);
  return true;
}

bool nyir_copy_prop(nyir_func_t *f) {
  if (!f || f->next_value <= 0)
    return true;
  /*
   * The linear alias map below is sound only for straight-line NYIR; for
   * control flow the dominator-checked walk above keeps every rewrite on a
   * path where the copied source is actually available.
   */
  if (nyir_has_control_flow(f))
    return copy_prop_cfg(f);
  size_t nv = (size_t)f->next_value;
  int stk_alias[256];
  int *alias = nv <= 256 ? stk_alias : (int *)malloc(nv * sizeof(int));
  if (!alias)
    return false;
  for (int i = 0; i < f->next_value; ++i)
    alias[i] = i;
  for (size_t i = 0; i < f->len; ++i) {
    nyir_inst_t *in = &f->data[i];
    if (in->a >= 0)
      in->a = nir_alias_find(alias, in->a);
    if (in->b >= 0)
      in->b = nir_alias_find(alias, in->b);
    if (in->c >= 0)
      in->c = nir_alias_find(alias, in->c);
    if (in->d >= 0)
      in->d = nir_alias_find(alias, in->d);
    if (in->e >= 0)
      in->e = nir_alias_find(alias, in->e);
    if (in->f >= 0)
      in->f = nir_alias_find(alias, in->f);
    for (size_t k = 0; k < in->extra_args_len; ++k) {
      if (in->extra_args[k] >= 0)
        in->extra_args[k] = nir_alias_find(alias, in->extra_args[k]);
    }
    for (size_t k = 0; k < in->phi_incoming_len; ++k)
      if (in->phi_incoming[k].value >= 0)
        in->phi_incoming[k].value =
            nir_alias_find(alias, in->phi_incoming[k].value);
    if (in->dst >= 0) {
      if (in->op == NYIR_COPY && in->a >= 0)
        alias[in->dst] = nir_alias_find(alias, in->a);
      else
        alias[in->dst] = in->dst;
    }
  }
  if (nv > 256) free(alias);
  return true;
}
