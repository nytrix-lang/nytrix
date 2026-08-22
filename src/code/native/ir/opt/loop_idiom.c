/*
 * Narrow counted-loop idioms that are exact under NyIR wrapping i64 semantics.
 */
#include "code/native/ir/opt/loop_analysis.h"
#include "code/native/ir/opt/util.h"
#include <stdlib.h>
#include <string.h>

static int root_copy(const nyir_func_t *f, const int *defs, int value) {
  while (value >= 0 && value < f->next_value && defs[value] >= 0 &&
         f->data[defs[value]].op == NYIR_COPY)
    value = f->data[defs[value]].a;
  return value;
}

static bool const_i64_val(const nyir_func_t *f, const int *defs, int value,
                           int64_t *out) {
  value = root_copy(f, defs, value);
  if (value < 0 || value >= f->next_value || defs[value] < 0)
    return false;
  const nyir_inst_t *def = &f->data[defs[value]];
  if (def->op != NYIR_CONST_I64)
    return false;
  if (out)
    *out = def->imm;
  return true;
}

typedef struct {
  size_t pos;
  int src;
  int shift;
} nir_loop_idiom_mul_t;

/*
 * Canonicalize `iv * C` (C a power of 2 >= 2) to `iv << log2(C)` inside
 * a counted positive-step natural loop.  Exact under Nytrix's wrapping i64
 * multiply for any shift in [1, 63): (iv * 2^s) mod 2^64 == (iv << s).
 *
 *   iv * 2  -> iv << 1
 *   iv * 4  -> iv << 2
 *   ... up to iv * (1<<62) -> iv << 62
 *
 * For non-power-of-2 multipliers, leave them for general strength-reduce.
 *
 * Candidates are collected first and applied in descending instruction
 * position, so inserting a CONST before a MUL never invalidates the
 * position of a MUL that has not been rewritten yet (the old approach of
 * bumping cfg.block_end per insertion left the block ranges of every later
 * block stale and silently skipped MULs near block ends).
 */
bool nyir_loop_idiom(nyir_func_t *f) {
  if (!f || f->next_value <= 0)
    return true;
  nyir_cfg_t cfg = {0};
  nyir_scev_info_t info = {0};
  int *defs = nyir_build_defs(f);
  if (!defs || !nyir_cfg_build(f, &cfg) || !nyir_scev_analyze(f, &info)) {
    free(defs);
    nyir_cfg_free(&cfg);
    nyir_scev_free(&info);
    return false;
  }
  bool *in_loop = calloc(cfg.block_count, sizeof(*in_loop));
  if (!in_loop) {
    free(defs);
    nyir_cfg_free(&cfg);
    nyir_scev_free(&info);
    return false;
  }
  nir_loop_idiom_mul_t *cands = NULL;
  size_t cand_count = 0, cand_cap = 0;
  for (size_t li = 0; li < info.count; ++li) {
    const nyir_scev_loop_t *loop = &info.loops[li];
    if (loop->step <= 0 || !loop->limit_is_const ||
        !nyir_cfg_natural_loop_blocks(&cfg, loop->latch_block,
                                       loop->header_block, in_loop,
                                       cfg.block_count))
      continue;
    for (size_t block = 0; block < cfg.block_count; ++block) {
      if (!in_loop[block])
        continue;
      for (size_t i = cfg.block_start[block]; i < cfg.block_end[block]; ++i) {
        const nyir_inst_t *in = &f->data[i];
        if (in->op != NYIR_MUL_I64)
          continue;
        /*
         * Identify the iv operand and the constant multiplier.
         */
        int iv_operand = -1;
        int64_t multiplier = 0;
        if (root_copy(f, defs, in->a) == loop->iv &&
            const_i64_val(f, defs, in->b, &multiplier)) {
          iv_operand = in->a;
        } else if (root_copy(f, defs, in->b) == loop->iv &&
                   const_i64_val(f, defs, in->a, &multiplier)) {
          iv_operand = in->b;
        }
        if (iv_operand < 0 || multiplier <= 1)
          continue;
        /*
         * Only handle exact powers of two.
         */
        if (multiplier <= 0 || (multiplier & (multiplier - 1)) != 0)
          continue;
        int shift = 0;
        int64_t v = multiplier;
        while (v > 1) { v >>= 1; shift++; }
        if (shift < 1 || shift > 62)
          continue;
        if (cand_count == cand_cap) {
          size_t new_cap = cand_cap ? cand_cap * 2 : 16;
          nir_loop_idiom_mul_t *grown = realloc(cands, new_cap * sizeof(*grown));
          if (!grown) {
            free(cands);
            free(in_loop);
            free(defs);
            nyir_cfg_free(&cfg);
            nyir_scev_free(&info);
            return false;
          }
          cands = grown;
          cand_cap = new_cap;
        }
        cands[cand_count++] = (nir_loop_idiom_mul_t){.pos = i,
                                                     .src = iv_operand,
                                                     .shift = shift};
      }
    }
  }
  for (size_t ci = cand_count; ci > 0; --ci) {
    const nir_loop_idiom_mul_t *c = &cands[ci - 1];
    size_t i = c->pos;
    /*
     * Another candidate may already have rewritten this position (nested
     * loops can match the same MUL twice); only rewrite a live MUL.
     */
    if (i >= f->len || f->data[i].op != NYIR_MUL_I64)
      continue;
    int shift_dst = f->next_value++;
    if (!nir_ensure_inst_space(f, 1)) {
      free(cands);
      free(in_loop);
      free(defs);
      nyir_cfg_free(&cfg);
      nyir_scev_free(&info);
      return false;
    }
    memmove(&f->data[i + 1], &f->data[i], (f->len - i) * sizeof(*f->data));
    f->len++;
    f->data[i] = (nyir_inst_t){.op = NYIR_CONST_I64,
                                .dst = shift_dst,
                                .a = -1, .b = -1, .c = -1, .d = -1,
                                .e = -1, .f = -1,
                                .imm = (int64_t)c->shift};
    nyir_inst_t *mul = &f->data[i + 1];
    mul->op = NYIR_SHL_I64;
    mul->a = c->src;
    mul->b = shift_dst;
    mul->effects = nyir_inst_effects(mul);
  }
  free(cands);
  free(in_loop);
  free(defs);
  nyir_cfg_free(&cfg);
  nyir_scev_free(&info);
  return true;
}
