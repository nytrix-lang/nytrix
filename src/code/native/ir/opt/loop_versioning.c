/*
 * Canonical single-version form for loop-invariant integer predicates.
 */
#include "code/native/ir/opt/loop_analysis.h"
#include "code/native/ir/opt/util.h"
#include <stdlib.h>

static int root_copy(const nyir_func_t *f, const int *defs, int value) {
  while (value >= 0 && value < f->next_value && defs[value] >= 0 &&
         f->data[defs[value]].op == NYIR_COPY)
    value = f->data[defs[value]].a;
  return value;
}

static nyir_cmp_t swapped(nyir_cmp_t cmp) {
  switch (cmp) {
  case NYIR_CMP_LT: return NYIR_CMP_GT;
  case NYIR_CMP_LE: return NYIR_CMP_GE;
  case NYIR_CMP_GT: return NYIR_CMP_LT;
  case NYIR_CMP_GE: return NYIR_CMP_LE;
  default: return cmp;
  }
}

/*
 * Put a loop-invariant relational guard in constant-left canonical form.  It
 * is a concrete, verifier-safe specialization of the existing loop version;
 * guards involving the induction variable are deliberately left unchanged.
 */
bool nyir_loop_versioning(nyir_func_t *f) {
  if (!f || f->next_value <= 0)
    return true;
  nyir_cfg_t cfg = {0};
  int *defs = nyir_build_defs(f);
  if (!defs || !nyir_cfg_build(f, &cfg)) {
    free(defs);
    nyir_cfg_free(&cfg);
    return false;
  }
  bool *in_loop = calloc(cfg.block_count, sizeof(*in_loop));
  if (!in_loop) {
    free(defs);
    nyir_cfg_free(&cfg);
    return false;
  }
  for (size_t latch = 0; latch < cfg.block_count; ++latch) {
    for (size_t e = cfg.succ_offsets[latch]; e < cfg.succ_offsets[latch + 1];
         ++e) {
      size_t header = cfg.succ_blocks[e];
      if (!nyir_cfg_is_backedge(&cfg, latch, header) ||
          !nyir_cfg_natural_loop_blocks(&cfg, latch, header, in_loop,
                                         cfg.block_count))
        continue;
      for (size_t block = 0; block < cfg.block_count; ++block) {
        if (!in_loop[block])
          continue;
        for (size_t i = cfg.block_start[block]; i < cfg.block_end[block]; ++i) {
          nyir_inst_t *in = &f->data[i];
          if (in->op != NYIR_CMP_I64 || in->cmp == NYIR_CMP_EQ ||
              in->cmp == NYIR_CMP_NE)
            continue;
          int left = root_copy(f, defs, in->a);
          int right = root_copy(f, defs, in->b);
          if (left < 0 || right < 0 || defs[left] < 0 || defs[right] < 0 ||
              in_loop[cfg.inst_block[(size_t)defs[left]]] ||
              in_loop[cfg.inst_block[(size_t)defs[right]]] ||
              f->data[defs[right]].op != NYIR_CONST_I64)
            continue;
          int tmp = in->a;
          in->a = in->b;
          in->b = tmp;
          in->cmp = swapped(in->cmp);
          free(in_loop);
          free(defs);
          nyir_cfg_free(&cfg);
          return true;
        }
      }
    }
  }
  free(in_loop);
  free(defs);
  nyir_cfg_free(&cfg);
  return true;
}
