/*
 * Minimal, proof-complete loop interchange for empty rectangular nests.
 */
#include "code/native/ir/opt/loop_analysis.h"
#include "code/native/ir/opt/util.h"
#include <stdlib.h>

static int root_copy(const nyir_func_t *f, const int *defs, int value) {
  for (int depth = 0; value >= 0 && value < f->next_value &&
                      defs[value] >= 0 &&
                      f->data[defs[value]].op == NYIR_COPY && depth < 32;
       ++depth)
    value = f->data[defs[value]].a;
  return value;
}

static bool is_iv_control_use(const nyir_inst_t *in, int iv, int next) {
  if (in->op == NYIR_PHI || in->op == NYIR_COPY)
    return true;
  if (in->op == NYIR_CMP_I64 && in->a == iv)
    return true;
  if ((in->op == NYIR_ADD_I64 || in->op == NYIR_SUB_I64) && in->dst == next)
    return true;
  return false;
}

static bool iv_has_only_control_uses(const nyir_func_t *f, const int *defs,
                                     const nyir_scev_loop_t *outer,
                                     const nyir_scev_loop_t *inner) {
  for (size_t i = 0; i < f->len; ++i) {
    const nyir_inst_t *in = &f->data[i];
    if (in->op == NYIR_PHI)
      continue;
    nyir_inst_t canonical = *in;
    canonical.a = root_copy(f, defs, canonical.a);
    canonical.b = root_copy(f, defs, canonical.b);
    canonical.c = root_copy(f, defs, canonical.c);
    canonical.d = root_copy(f, defs, canonical.d);
    canonical.e = root_copy(f, defs, canonical.e);
    canonical.f = root_copy(f, defs, canonical.f);
    bool uses_outer = canonical.a == outer->iv || canonical.b == outer->iv ||
                      canonical.c == outer->iv || canonical.d == outer->iv ||
                      canonical.e == outer->iv || canonical.f == outer->iv;
    bool uses_inner = canonical.a == inner->iv || canonical.b == inner->iv ||
                      canonical.c == inner->iv || canonical.d == inner->iv ||
                      canonical.e == inner->iv || canonical.f == inner->iv;
    if (uses_outer &&
        !is_iv_control_use(&canonical, outer->iv, outer->next_value))
      return false;
    if (uses_inner &&
        !is_iv_control_use(&canonical, inner->iv, inner->next_value))
      return false;
  }
  return true;
}

static nyir_inst_t *loop_cmp(nyir_func_t *f, const nyir_cfg_t *cfg,
                             const int *defs, const nyir_scev_loop_t *l) {
  if (!f || !cfg || !l || l->header_block >= cfg->block_count)
    return NULL;
  for (size_t i = cfg->block_start[l->header_block];
       i < cfg->block_end[l->header_block]; ++i)
    if (f->data[i].op == NYIR_CMP_I64 &&
        root_copy(f, defs, f->data[i].a) == l->iv &&
        root_copy(f, defs, f->data[i].b) ==
            root_copy(f, defs, l->limit_value))
      return &f->data[i];
  return NULL;
}

/*
 * Interchanging an empty rectangular nest is observable in the IR/CFG and in
 * trip ordering, but cannot change program data.  Requiring unused IVs, equal
 * starts/steps/predicates, and constant bounds is the smallest interchange the
 * current scalar NyIR can prove without an affine memory descriptor.
 */
bool nyir_loop_interchange(nyir_func_t *f) {
  if (!f)
    return true;
  nyir_scev_info_t info = {0};
  nyir_cfg_t cfg = {0};
  int *defs = nyir_build_defs(f);
  if (!defs || !nyir_scev_analyze(f, &info) || !nyir_cfg_build(f, &cfg)) {
    free(defs);
    nyir_scev_free(&info);
    nyir_cfg_free(&cfg);
    return false;
  }
  bool *outer_blocks = calloc(cfg.block_count, sizeof(*outer_blocks));
  bool *inner_blocks = calloc(cfg.block_count, sizeof(*inner_blocks));
  if (!outer_blocks || !inner_blocks) {
    free(outer_blocks);
    free(inner_blocks);
    free(defs);
    nyir_scev_free(&info);
    nyir_cfg_free(&cfg);
    return false;
  }
  for (size_t oi = 0; oi < info.count; ++oi) {
    nyir_scev_loop_t *outer = &info.loops[oi];
    if (!nyir_cfg_natural_loop_blocks(&cfg, outer->latch_block,
                                       outer->header_block, outer_blocks,
                                       cfg.block_count))
      continue;
    for (size_t ii = 0; ii < info.count; ++ii) {
      nyir_scev_loop_t *inner = &info.loops[ii];
      if (outer == inner ||
          !nyir_cfg_natural_loop_blocks(&cfg, inner->latch_block,
                                         inner->header_block, inner_blocks,
                                         cfg.block_count))
        continue;
      bool nested = inner->header_block != outer->header_block;
      for (size_t block = 0; block < cfg.block_count && nested; ++block)
        if (inner_blocks[block] && !outer_blocks[block])
          nested = false;
      if (!nested || outer->init != inner->init ||
          outer->step != inner->step || outer->predicate != inner->predicate ||
          !outer->limit_is_const || !inner->limit_is_const ||
          outer->limit == inner->limit ||
          !iv_has_only_control_uses(f, defs, outer, inner))
        continue;
      nyir_inst_t *outer_cmp = loop_cmp(f, &cfg, defs, outer);
      nyir_inst_t *inner_cmp = loop_cmp(f, &cfg, defs, inner);
      if (!outer_cmp || !inner_cmp)
        continue;
      int outer_limit = root_copy(f, defs, outer_cmp->b);
      int inner_limit = root_copy(f, defs, inner_cmp->b);
      int outer_def = outer_limit >= 0 ? defs[outer_limit] : -1;
      int inner_def = inner_limit >= 0 ? defs[inner_limit] : -1;
      if (outer_def < 0 || inner_def < 0 ||
          f->data[outer_def].op != NYIR_CONST_I64 ||
          f->data[inner_def].op != NYIR_CONST_I64)
        continue;
      int64_t limit = f->data[outer_def].imm;
      f->data[outer_def].imm = f->data[inner_def].imm;
      f->data[inner_def].imm = limit;
      free(outer_blocks);
      free(inner_blocks);
      free(defs);
      nyir_scev_free(&info);
      nyir_cfg_free(&cfg);
      return true;
    }
  }
  free(outer_blocks);
  free(inner_blocks);
  free(defs);
  nyir_scev_free(&info);
  nyir_cfg_free(&cfg);
  return true;
}
