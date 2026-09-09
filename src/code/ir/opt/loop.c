/*
 * Compact scalar loop transforms; large pipeline and vector passes live in
 * loop_pipeline.c and loop_vectorize.c. Shared analysis is declared by loop.h.
 */

#include "code/ir/opt/loop.h"
#include "code/ir/opt/util.h"
#include "code/ir/internal.h"
#include "base/compat.h"
#include "base/common.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Narrow counted-loop idioms that are exact under NyIR wrapping i64 semantics.
 */

static int idiom_root_copy(const nyir_func_t *f, const int *defs, int value) {
  while (value >= 0 && value < f->next_value && defs[value] >= 0 &&
         f->data[defs[value]].op == NYIR_COPY)
    value = f->data[defs[value]].a;
  return value;
}


static bool const_i64_val(const nyir_func_t *f, const int *defs, int value,
                           int64_t *out) {
  value = idiom_root_copy(f, defs, value);
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
        if (idiom_root_copy(f, defs, in->a) == loop->iv &&
            const_i64_val(f, defs, in->b, &multiplier)) {
          iv_operand = in->a;
        } else if (idiom_root_copy(f, defs, in->b) == loop->iv &&
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
    mul->flags = 0;
    mul->effects = nyir_inst_effects(mul);
  }
  free(cands);
  free(in_loop);
  free(defs);
  nyir_cfg_free(&cfg);
  nyir_scev_free(&info);
  /*
   * The DNA-complement rewrite (3 - x → x ^ 3) was removed: the
   * identity holds only for 2-bit bases (x in [0,3]) and only for the
   * const-on-the-left orientation, none of which is provable from
   * local pattern shape. Applied to an arbitrary loaded value it
   * changed results outright.
   */
  return true;
}

/*
 * Minimal, proof-complete loop interchange for empty rectangular nests.
 */

static int interchange_root_copy(const nyir_func_t *f, const int *defs, int value) {
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
    canonical.a = interchange_root_copy(f, defs, canonical.a);
    canonical.b = interchange_root_copy(f, defs, canonical.b);
    canonical.c = interchange_root_copy(f, defs, canonical.c);
    canonical.d = interchange_root_copy(f, defs, canonical.d);
    canonical.e = interchange_root_copy(f, defs, canonical.e);
    canonical.f = interchange_root_copy(f, defs, canonical.f);
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
        interchange_root_copy(f, defs, f->data[i].a) == l->iv &&
        interchange_root_copy(f, defs, f->data[i].b) ==
            interchange_root_copy(f, defs, l->limit_value))
      return &f->data[i];
  return NULL;
}

/*
 * The interchange swaps the IMM of each guard-limit CONST node. When CSE
 * deduplicated constants, the same node may feed unrelated instructions
 * (strides, sizes, later arithmetic); swapping its imm silently changes
 * those users. Require exactly one operand reference per limit constant.
 */
static bool const_has_single_use(const nyir_func_t *f, int def_idx) {
  int dst = f->data[def_idx].dst;
  size_t uses = 0;
  for (size_t i = 0; i < f->len && uses <= 1; ++i) {
    const nyir_inst_t *in = &f->data[i];
    const int refs[] = {in->a, in->b, in->c, in->d, in->e, in->f};
    for (size_t k = 0; k < sizeof(refs) / sizeof(refs[0]); ++k)
      if (refs[k] == dst)
        ++uses;
    for (size_t k = 0; k < in->extra_args_len; ++k)
      if (in->extra_args[k] == dst)
        ++uses;
  }
  return uses == 1;
}

/*
 * Interchange reorders the iteration schedule, which is only safe for
 * stateless bodies. Reject memory writes, calls, and any loop-carried PHI
 * beyond the two induction variables (coupled recurrences such as
 * x += y / y *= 2 reorder under swap and change results).
 */
static bool bodies_stateless(const nyir_func_t *f, const nyir_cfg_t *cfg,
                             const bool *outer_blocks, const bool *inner_blocks,
                             int outer_iv, int inner_iv) {
  for (size_t b = 0; b < cfg->block_count; ++b) {
    if (!outer_blocks[b] && !inner_blocks[b])
      continue;
    for (size_t i = cfg->block_start[b]; i < cfg->block_end[b]; ++i) {
      const nyir_inst_t *in = &f->data[i];
      switch (in->op) {
      case NYIR_CALL:
      case NYIR_STORE_I64:
      case NYIR_STORE_LOCAL:
      case NYIR_COPY_STRUCT:
      case NYIR_VEC4_STORE_F64:
      case NYIR_VEC8_STORE_F32:
      case NYIR_VEC4_STORE_I64:
      case NYIR_VEC8_STORE_I64:
        return false;
      case NYIR_PHI:
        if (in->dst != outer_iv && in->dst != inner_iv)
          return false;
        break;
      default:
        break;
      }
    }
  }
  return true;
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
      int outer_limit = interchange_root_copy(f, defs, outer_cmp->b);
      int inner_limit = interchange_root_copy(f, defs, inner_cmp->b);
      int outer_def = outer_limit >= 0 ? defs[outer_limit] : -1;
      int inner_def = inner_limit >= 0 ? defs[inner_limit] : -1;
      if (outer_def < 0 || inner_def < 0 ||
          f->data[outer_def].op != NYIR_CONST_I64 ||
          f->data[inner_def].op != NYIR_CONST_I64)
        continue;
      if (!const_has_single_use(f, outer_def) ||
          !const_has_single_use(f, inner_def))
        continue;
      if (!bodies_stateless(f, &cfg, outer_blocks, inner_blocks,
                            outer->iv, inner->iv))
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

/*
 * Unroll-and-jam for canonical two-block counted loops.
 *
 * A canonical loop {header, latch} with a pure-scalar body and a known, even
 * trip count is unrolled by a factor of two inside the loop: each trip runs
 * the first body at `iv` and a fetched clone of the body at `iv + 1`, then
 * advances the induction by two.  The header guard (iv < T) is unchanged; the
 * step-2 induction halves the trip naturally and runs exactly the original T
 * body executions in order.
 *
 *   Before (step 1, T even):
 *     H: iv=phi(0, back); c=cmp(iv,T); br_if c -> L
 *     L: body(iv,...); next=phiBack(body outputs); iv'=iv+1; br -> H
 *
 *   After (step 2):
 *     H: iv=phi(0, back); c=cmp(iv,T); br_if c -> L
 *     L: body(iv,...); body(iv+1, carriedNext,...); next2=...; br -> H
 *
 * Correctness: the second body's uses of each header PHI value map to the
 * first body's carried output, and its own step writes iv+2; the guard still
 * runs while iv < T, i.e. T/2 trips of two bodies each.  Loop-carried values
 * flow through the first body into the second, preserving order and data
 * dependencies.
 *
 * Constraints: pure-scalar payload-free latch body (no memory/calls/traps/
 * control), SCEV-proven even trip count, canonical LT guard on the SCEV
 * induction with constant trip bound, step +1, exactly two natural-loop
 * blocks.
 *
 * Transactional: operates on a clone and commits only after nyir_verify.
 */


static size_t jam_last_non_nop(const nyir_func_t *f, const nyir_cfg_t *cfg,
                               size_t block) {
  size_t end = cfg->block_end[block];
  while (end > cfg->block_start[block] && f->data[end - 1].op == NYIR_NOP)
    --end;
  return end;
}

static bool jam_payload_free(const nyir_inst_t *in) {
  return in && !in->symbol && !in->debug.file && !in->extra_args &&
         !in->arg_sizes && !in->phi_incoming;
}

static bool jam_body_pure(const nyir_inst_t *in) {
  if (!in || !jam_payload_free(in))
    return false;
  if (nyir_inst_effects(in) != NYIR_EFFECT_NONE)
    return false;
  switch (in->op) {
  case NYIR_ADD_I64:
  case NYIR_SUB_I64:
  case NYIR_MUL_I64:
  case NYIR_DIV_I64:
  case NYIR_MOD_I64:
  case NYIR_AND_I64:
  case NYIR_OR_I64:
  case NYIR_XOR_I64:
  case NYIR_SHL_I64:
  case NYIR_SAR_I64:
  case NYIR_COPY:
  case NYIR_CONST_I64:
  case NYIR_SELECT_I64:
    return true;
  default:
    return false;
  }
}

static int jam_map_value(const int *map, size_t map_len, int value) {
  if (value < 0)
    return value;
  if ((size_t)value < map_len && map[value] >= 0)
    return map[value];
  return value;
}

static bool jam_try_one(nyir_func_t *work, const nyir_scev_loop_t *loop) {
  if (!work || !loop || !loop->trip_count_known || loop->trip_count < 2 ||
      (loop->trip_count & 1) != 0 || loop->step != 1)
    return false;

  nyir_cfg_t cfg = {0};
  bool *in_loop = NULL;
  int *map = NULL;
  bool changed = false;
  if (!nyir_cfg_build(work, &cfg))
    goto out;
  if (loop->header_block >= cfg.block_count ||
      loop->latch_block >= cfg.block_count ||
      loop->preheader_block >= cfg.block_count ||
      loop->header_block == loop->latch_block)
    goto out;

  in_loop = calloc(cfg.block_count, sizeof(*in_loop));
  if (!in_loop ||
      !nyir_cfg_natural_loop_blocks(&cfg, loop->latch_block,
                                    loop->header_block, in_loop,
                                    cfg.block_count))
    goto out;
  size_t loop_blocks = 0;
  for (size_t b = 0; b < cfg.block_count; ++b)
    loop_blocks += in_loop[b] ? 1u : 0u;
  if (loop_blocks != 2 || !in_loop[loop->header_block] ||
      !in_loop[loop->latch_block])
    goto out;

  const size_t h = loop->header_block, l = loop->latch_block;
  const size_t p = loop->preheader_block;
  int64_t hlabel = cfg.block_label[h];
  int64_t llabel = cfg.block_label[l];
  if (hlabel < 0 || llabel < 0)
    goto out;

  size_t pend = jam_last_non_nop(work, &cfg, p);
  size_t hend = jam_last_non_nop(work, &cfg, h);
  size_t lend = jam_last_non_nop(work, &cfg, l);
  if (pend <= cfg.block_start[p] || hend <= cfg.block_start[h] ||
      lend <= cfg.block_start[l])
    goto out;
  nyir_inst_t *pterm = &work->data[pend - 1];
  const nyir_inst_t *hterm = &work->data[hend - 1];
  const nyir_inst_t *lterm = &work->data[lend - 1];
  if (pterm->op != NYIR_BR || pterm->imm != hlabel ||
      hterm->op != NYIR_BR_IF || lterm->op != NYIR_BR ||
      lterm->imm != hlabel)
    goto out;
  size_t taken = SIZE_MAX;
  for (size_t b = 0; b < cfg.block_count; ++b)
    if (cfg.block_label[b] == hterm->imm) {
      taken = b;
      break;
    }
  if (taken != l)
    goto out;

  /*
   * Header scan: PHIs, then one LT cmp(iv, const), then the br_if at hend-1.
   */
  size_t phi_begin = cfg.block_start[h];
  if (phi_begin < cfg.block_end[h] && work->data[phi_begin].op == NYIR_LABEL)
    ++phi_begin;
  while (phi_begin < cfg.block_end[h] && work->data[phi_begin].op == NYIR_NOP)
    ++phi_begin;

  size_t map_len = (size_t)(work->next_value > 0 ? work->next_value : 1);
  map = malloc(map_len * sizeof(*map));
  if (!map)
    goto out;
  for (size_t i = 0; i < map_len; ++i)
    map[i] = -1;

  int phi_dst[32], phi_back[32];
  int phi_count = 0;
  bool saw_phi = false;
  int guard_iv = -1, guard_trip = -1;
  bool have_guard = false;
  for (size_t i = phi_begin; i < hend - 1; ++i) {
    nyir_inst_t *in = &work->data[i];
    if (in->op == NYIR_NOP)
      continue;
    if (in->op == NYIR_PHI) {
      saw_phi = true;
      if (in->dst < 0 || (size_t)in->dst >= map_len ||
          in->phi_incoming_len != 2 || phi_count >= 32)
        goto out;
      int back = -1;
      bool have_latch = false;
      for (size_t k = 0; k < in->phi_incoming_len; ++k) {
        const nyir_phi_incoming_t *inc = &in->phi_incoming[k];
        if (inc->predecessor_label == llabel) {
          back = inc->value;
          have_latch = true;
        }
      }
      if (!have_latch || back < 0)
        goto out;
      phi_dst[phi_count] = in->dst;
      phi_back[phi_count] = back;
      ++phi_count;
      continue;
    }
    if (in->op == NYIR_CMP_I64) {
      if (have_guard || in->cmp != NYIR_CMP_LT || in->a < 0 || in->b < 0)
        goto out;
      guard_iv = in->a;
      guard_trip = in->b;
      have_guard = true;
      continue;
    }
    goto out; /* unexpected instruction before the terminator */
  }
  if (!saw_phi || !have_guard)
    goto out;
  if (guard_iv != loop->iv)
    goto out;
  int64_t trip_imm = 0;
  bool trip_is_const = false;
  for (size_t i = 0; i <= pend; ++i) {
    if (work->data[i].op == NYIR_CONST_I64 &&
        work->data[i].dst == guard_trip) {
      trip_imm = work->data[i].imm;
      trip_is_const = true;
      break;
    }
  }
  if (!trip_is_const || trip_imm != (int64_t)loop->trip_count)
    goto out;

  /*
   * Latch body boundaries.
   */
  size_t body_begin = cfg.block_start[l];
  if (body_begin < cfg.block_end[l] && work->data[body_begin].op == NYIR_LABEL)
    ++body_begin;
  while (body_begin < lend - 1 && work->data[body_begin].op == NYIR_NOP)
    ++body_begin;
  if (body_begin >= lend - 1)
    goto out;
  for (size_t i = body_begin; i + 1 < lend; ++i) {
    const nyir_inst_t *in = &work->data[i];
    if (in->op == NYIR_NOP)
      continue;
    if (!jam_body_pure(in))
      goto out;
  }

  /*
   * Each carried value must be defined exactly once inside the latch body.
   */
  for (int q = 0; q < phi_count; ++q) {
    int count = 0;
    for (size_t i = body_begin; i + 1 < lend; ++i)
      if (work->data[i].dst == phi_back[q])
        ++count;
    if (count != 1)
      goto out;
  }

  /*
   * Jamming halves the executions of body definitions that are NOT
   * carried through a header PHI: after the rewrite, only the cloned
   * (odd-iteration) copy produces them on the final iteration, so any
   * use outside the latch body would read the second-to-last
   * iteration's value. Require every non-carried body def to be dead
   * outside its own body region (same discipline as softpipe's
   * stage-A liveness check).
   */
  {
    bool carried[32];
    for (int q = 0; q < phi_count; ++q)
      carried[q] = true;
    for (size_t i = body_begin; i + 1 < lend; ++i) {
      const nyir_inst_t *in = &work->data[i];
      int d = in->dst;
      if (d < 0)
        continue;
      bool is_carried = false;
      for (int q = 0; q < phi_count; ++q)
        if (d == phi_back[q])
          is_carried = true;
      if (is_carried)
        continue;
      for (size_t j = 0; j < work->len; ++j) {
        if (j >= body_begin && j + 1 < lend)
          continue;
        const nyir_inst_t *user = &work->data[j];
        if (user->op == NYIR_NOP)
          continue;
        const int ops[6] = {user->a, user->b, user->c, user->d, user->e,
                            user->f};
        for (int o = 0; o < 6; ++o)
          if (ops[o] == d)
            goto out;
        for (size_t k = 0; k < user->extra_args_len; ++k)
          if (user->extra_args[k] == d)
            goto out;
        if (user->op == NYIR_PHI && user->phi_incoming) {
          for (size_t k = 0; k < user->phi_incoming_len; ++k)
            if (user->phi_incoming[k].value == d)
              goto out;
        }
      }
    }
  }

  /*
   * Mapping for the fetched second body: a use of a header PHI value (the
   * value at the start of this iteration) becomes the first body's carried
   * output for that PHI, i.e. the value at the start of the next iteration.
   */
  for (int q = 0; q < phi_count; ++q)
    map[phi_dst[q]] = phi_back[q];

  int new_back[32];
  for (int q = 0; q < phi_count; ++q)
    new_back[q] = -1;

  /*
   * Insert the fetched clone before the latch terminator.
   */
  size_t insert_at = lend; /* position of the trailing br; insert before it */
  for (size_t i = body_begin; i + 1 < lend; ++i) {
    const nyir_inst_t *src = &work->data[i];
    if (src->op == NYIR_NOP)
      continue;
    nyir_inst_t in = *src;
    int old_dst = in.dst;
    if (in.dst >= 0) {
      if ((size_t)in.dst >= map_len || work->next_value == INT_MAX)
        goto out;
      int fresh = work->next_value++;
      map[in.dst] = fresh;
      in.dst = fresh;
    }
    in.a = jam_map_value(map, map_len, in.a);
    in.b = jam_map_value(map, map_len, in.b);
    in.c = jam_map_value(map, map_len, in.c);
    in.d = jam_map_value(map, map_len, in.d);
    in.e = jam_map_value(map, map_len, in.e);
    in.f = jam_map_value(map, map_len, in.f);
    if (!nir_ensure_inst_space(work, 1))
      goto out;
    memmove(&work->data[insert_at + 1], &work->data[insert_at],
            (work->len - insert_at) * sizeof(*work->data));
    work->data[insert_at] = in;
    work->data[insert_at].effects =
        nyir_inst_effects(&work->data[insert_at]);
    work->len += 1;
    ++insert_at;
    if (old_dst >= 0)
      for (int q = 0; q < phi_count; ++q)
        if (old_dst == phi_back[q])
          new_back[q] = (int)map[phi_back[q]];
  }

  /*
   * Wire the header PHIs to the fetched body's outputs.
   */
  for (int q = 0; q < phi_count; ++q) {
    if (new_back[q] < 0)
      goto out;
    if ((size_t)new_back[q] >= map_len) {
      /*
       * Allocate a fresh id from map[phi_back] only when mapped.
       */
      continue;
    }
    for (size_t i = phi_begin; i < hend - 1; ++i) {
      nyir_inst_t *phi = &work->data[i];
      if (phi->op != NYIR_PHI || phi->dst != phi_dst[q])
        continue;
      for (size_t k = 0; k < phi->phi_incoming_len; ++k) {
        nyir_phi_incoming_t *inc = &phi->phi_incoming[k];
        if (inc->predecessor_label == llabel) {
          inc->value = new_back[q];
        }
      }
    }
  }

  char err[256] = {0};
  changed = nyir_verify(work, err, sizeof(err));

out:
  free(map);
  free(in_loop);
  nyir_cfg_free(&cfg);
  return changed;
}

bool nyir_unroll_jam(nyir_func_t *f) {
  if (!f || f->len == 0)
    return true;

  nyir_scev_info_t info = {0};
  if (!nyir_scev_analyze(f, &info))
    return false;

  for (size_t i = 0; i < info.count; ++i) {
    const nyir_scev_loop_t *loop = &info.loops[i];
    if (!loop->trip_count_known || loop->trip_count < 2 ||
        (loop->trip_count & 1) != 0 || loop->limit_value < 0 ||
        loop->step != 1)
      continue;

    nyir_func_t candidate = {0};
    if (!nyir_func_clone(f, &candidate)) {
      nyir_scev_free(&info);
      return false;
    }
    if (jam_try_one(&candidate, loop)) {
      char err[256] = {0};
      if (nyir_verify(&candidate, err, sizeof(err))) {
        nyir_func_free(f);
        *f = candidate;
        nyir_scev_free(&info);
        return true;
      }
    }
    nyir_func_free(&candidate);
  }

  nyir_scev_free(&info);
  return true;
}
/*
 * First-iteration loop peeling for canonical two-block counted loops.
 *
 * The transform is intentionally transactional: operate on a deep clone and
 * commit only after the NYIR verifier accepts the rewritten CFG/SSA graph.
 * It accepts a loop only when SCEV proves at least one iteration, the natural
 * loop is exactly {header,latch}, the preheader/latch both branch directly to
 * the header, and the latch body contains no nested control flow or owned
 * instruction payloads.  Memory effects are allowed: a positive trip count
 * proves the original guard executes the first body iteration as well.
 *
 * Before:
 *   P -> H(phi init from P, backedge from L)
 *   H --true--> L --back--> H
 *
 * After:
 *   P -> PEEL(init-substituted clone of L) -> H
 *   H(phi first value from PEEL, backedge from L)
 */


static size_t peel_last_non_nop(const nyir_func_t *f, const nyir_cfg_t *cfg,
                                size_t block) {
  size_t end = cfg->block_end[block];
  while (end > cfg->block_start[block] && f->data[end - 1].op == NYIR_NOP)
    --end;
  return end;
}

static int64_t peel_fresh_label(const nyir_func_t *f) {
  int64_t max_label = -1;
  for (size_t i = 0; i < f->len; ++i) {
    if ((f->data[i].op == NYIR_LABEL || f->data[i].op == NYIR_BR ||
         f->data[i].op == NYIR_BR_IF) &&
        f->data[i].imm > max_label)
      max_label = f->data[i].imm;
    for (size_t k = 0; k < f->data[i].phi_incoming_len; ++k)
      if (f->data[i].phi_incoming[k].predecessor_label > max_label)
        max_label = f->data[i].phi_incoming[k].predecessor_label;
  }
  return max_label == INT64_MAX ? -1 : max_label + 1;
}

static bool peel_payload_free(const nyir_inst_t *in) {
  return in && !in->symbol && !in->debug.file && !in->extra_args &&
         !in->arg_sizes && !in->phi_incoming;
}

/*
 * Peeling moves the latch body AHEAD of the header prefix. When BOTH
 * sides touch observable state (raw memory, local slots, calls), the
 * move reorders a store/load pair across the boundary and aliasing
 * programs observe a different final memory state. Rejecting only when
 * both sides touch state keeps every pure-guard peel legal.
 */
static bool peel_touches_state(const nyir_inst_t *in) {
  unsigned e = nyir_inst_effects(in);
  return (e & (NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_WRITE_MEMORY |
               NYIR_EFFECT_READ_LOCAL | NYIR_EFFECT_WRITE_LOCAL |
               NYIR_EFFECT_UNKNOWN_SIDE_EFFECT | NYIR_EFFECT_VOLATILE)) != 0;
}

static int peel_map_value(const int *map, size_t map_len, int value) {
  if (value < 0)
    return value;
  if ((size_t)value < map_len && map[value] >= 0)
    return map[value];
  return value;
}

static bool peel_try_one(nyir_func_t *work, const nyir_scev_loop_t *loop) {
  if (!work || !loop || !loop->trip_count_known || loop->trip_count == 0)
    return false;

  nyir_cfg_t cfg = {0};
  bool *in_loop = NULL;
  int *map = NULL;
  nyir_inst_t *clone = NULL;
  bool changed = false;
  if (!nyir_cfg_build(work, &cfg))
    goto out;
  if (loop->header_block >= cfg.block_count ||
      loop->latch_block >= cfg.block_count ||
      loop->preheader_block >= cfg.block_count ||
      loop->header_block == loop->latch_block)
    goto out;

  in_loop = calloc(cfg.block_count, sizeof(*in_loop));
  if (!in_loop ||
      !nyir_cfg_natural_loop_blocks(&cfg, loop->latch_block,
                                    loop->header_block, in_loop,
                                    cfg.block_count))
    goto out;
  size_t loop_blocks = 0;
  for (size_t b = 0; b < cfg.block_count; ++b)
    loop_blocks += in_loop[b] ? 1u : 0u;
  if (loop_blocks != 2 || !in_loop[loop->header_block] ||
      !in_loop[loop->latch_block])
    goto out;

  const size_t h = loop->header_block, l = loop->latch_block;
  const size_t p = loop->preheader_block;
  int64_t hlabel = cfg.block_label[h];
  int64_t plabel = cfg.block_label[p];
  int64_t llabel = cfg.block_label[l];
  if (hlabel < 0 || llabel < 0)
    goto out;

  size_t pend = peel_last_non_nop(work, &cfg, p);
  size_t hend = peel_last_non_nop(work, &cfg, h);
  size_t lend = peel_last_non_nop(work, &cfg, l);
  if (pend <= cfg.block_start[p] || hend <= cfg.block_start[h] ||
      lend <= cfg.block_start[l])
    goto out;
  nyir_inst_t *pterm = &work->data[pend - 1];
  const nyir_inst_t *hterm = &work->data[hend - 1];
  const nyir_inst_t *lterm = &work->data[lend - 1];
  if (pterm->op != NYIR_BR || pterm->imm != hlabel ||
      hterm->op != NYIR_BR_IF || lterm->op != NYIR_BR ||
      lterm->imm != hlabel)
    goto out;
  size_t taken = SIZE_MAX;
  for (size_t b = 0; b < cfg.block_count; ++b)
    if (cfg.block_label[b] == hterm->imm) {
      taken = b;
      break;
    }
  if (taken != l)
    goto out;

  /*
   * Header must contain only PHIs/NOPs before ordinary guard computation.
   */
  size_t phi_begin = cfg.block_start[h];
  if (phi_begin < cfg.block_end[h] && work->data[phi_begin].op == NYIR_LABEL)
    ++phi_begin;
  while (phi_begin < cfg.block_end[h] && work->data[phi_begin].op == NYIR_NOP)
    ++phi_begin;

  size_t map_len = (size_t)(work->next_value > 0 ? work->next_value : 1);
  map = malloc(map_len * sizeof(*map));
  if (!map)
    goto out;
  for (size_t i = 0; i < map_len; ++i)
    map[i] = -1;

  bool saw_phi = false;
  for (size_t i = phi_begin; i < hend - 1; ++i) {
    nyir_inst_t *phi = &work->data[i];
    if (phi->op == NYIR_NOP)
      continue;
    if (phi->op != NYIR_PHI)
      break;
    saw_phi = true;
    if (phi->dst < 0 || (size_t)phi->dst >= map_len ||
        phi->phi_incoming_len != 2)
      goto out;
    int init = -1;
    bool have_pre = false, have_latch = false;
    for (size_t k = 0; k < phi->phi_incoming_len; ++k) {
      const nyir_phi_incoming_t *inc = &phi->phi_incoming[k];
      if (inc->predecessor_label == plabel) {
        init = inc->value;
        have_pre = true;
      } else if (inc->predecessor_label == llabel) {
        have_latch = true;
      }
    }
    if (!have_pre || !have_latch || init < 0)
      goto out;
    map[phi->dst] = init;
  }
  if (!saw_phi)
    goto out;

  /*
   * Clone the latch body with first-iteration PHI substitutions.
   */
  size_t body_begin = cfg.block_start[l];
  if (body_begin < cfg.block_end[l] && work->data[body_begin].op == NYIR_LABEL)
    ++body_begin;
  while (body_begin < lend - 1 && work->data[body_begin].op == NYIR_NOP)
    ++body_begin;
  size_t body_cap = lend > body_begin ? lend - body_begin - 1 : 0;
  if (body_cap == 0)
    goto out;
  clone = calloc(body_cap, sizeof(*clone));
  if (!clone)
    goto out;
  size_t clone_len = 0;
  bool body_touches_state = false;
  for (size_t i = body_begin; i < lend - 1; ++i) {
    const nyir_inst_t *src = &work->data[i];
    if (src->op == NYIR_NOP)
      continue;
    if (src->op == NYIR_LABEL || src->op == NYIR_PHI || src->op == NYIR_BR ||
        src->op == NYIR_BR_IF || src->op == NYIR_RET ||
        !peel_payload_free(src))
      goto out;
    if (peel_touches_state(src))
      body_touches_state = true;
    nyir_inst_t in = *src;
    if (in.dst >= 0) {
      if ((size_t)in.dst >= map_len || work->next_value == INT_MAX)
        goto out;
      int fresh = work->next_value++;
      map[in.dst] = fresh;
      in.dst = fresh;
    }
    in.a = peel_map_value(map, map_len, in.a);
    in.b = peel_map_value(map, map_len, in.b);
    in.c = peel_map_value(map, map_len, in.c);
    in.d = peel_map_value(map, map_len, in.d);
    in.e = peel_map_value(map, map_len, in.e);
    in.f = peel_map_value(map, map_len, in.f);
    clone[clone_len++] = in;
  }
  if (clone_len == 0)
    goto out;

  /*
   * Header-prefix state check (see peel_touches_state): if the cloned
   * body AND the header prefix both touch observable state, the peel
   * would reorder them.
   */
  {
    bool prefix_touches_state = false;
    for (size_t i = phi_begin; i < hend - 1; ++i) {
      const nyir_inst_t *in = &work->data[i];
      if (in->op == NYIR_NOP || in->op == NYIR_PHI)
        continue;
      if (peel_touches_state(in))
        prefix_touches_state = true;
    }
    if (body_touches_state && prefix_touches_state)
      goto out;
  }

  int64_t peeled_label = peel_fresh_label(work);
  if (peeled_label < 0)
    goto out;

  /*
   * Rewire each header PHI to consume the value produced by the peel.
   */
  for (size_t i = phi_begin; i < hend - 1; ++i) {
    nyir_inst_t *phi = &work->data[i];
    if (phi->op == NYIR_NOP)
      continue;
    if (phi->op != NYIR_PHI)
      break;
    int back_value = -1;
    for (size_t k = 0; k < phi->phi_incoming_len; ++k)
      if (phi->phi_incoming[k].predecessor_label == llabel)
        back_value = phi->phi_incoming[k].value;
    if (back_value < 0)
      goto out;
    int first_result = peel_map_value(map, map_len, back_value);
    for (size_t k = 0; k < phi->phi_incoming_len; ++k) {
      nyir_phi_incoming_t *inc = &phi->phi_incoming[k];
      if (inc->predecessor_label == plabel) {
        inc->predecessor_label = peeled_label;
        inc->value = first_result;
      }
    }
  }
  pterm->imm = peeled_label;

  /*
   * Insert the peeled block immediately before the old header.
   */
  size_t insert = cfg.block_start[h];
  size_t add = clone_len + 2;
  if (!nir_ensure_inst_space(work, add))
    goto out;
  memmove(&work->data[insert + add], &work->data[insert],
          (work->len - insert) * sizeof(*work->data));
  work->len += add;
  work->data[insert] = (nyir_inst_t){.op = NYIR_LABEL, .dst = -1, .a = -1,
                                     .b = -1, .c = -1, .d = -1, .e = -1,
                                     .f = -1, .imm = peeled_label};
  memcpy(&work->data[insert + 1], clone, clone_len * sizeof(*clone));
  work->data[insert + 1 + clone_len] =
      (nyir_inst_t){.op = NYIR_BR, .dst = -1, .a = -1, .b = -1, .c = -1,
                    .d = -1, .e = -1, .f = -1, .imm = hlabel,
                    .effects = NYIR_EFFECT_CONTROL};

  char err[256] = {0};
  changed = nyir_verify(work, err, sizeof(err));

out:
  free(clone);
  free(map);
  free(in_loop);
  nyir_cfg_free(&cfg);
  return changed;
}

bool nyir_loop_peel(nyir_func_t *f) {
  if (!f || f->len == 0)
    return true;

  /*
   * Analyze the immutable source once.  Each candidate gets its own clone so
   * a rejected peel can never leak fresh SSA ids or partially rewritten PHIs
   * into the next candidate.
   */
  nyir_scev_info_t info = {0};
  if (!nyir_scev_analyze(f, &info))
    return false;

  for (size_t i = 0; i < info.count; ++i) {
    const nyir_scev_loop_t *loop = &info.loops[i];
    if (!loop->trip_count_known || loop->trip_count == 0 ||
        loop->limit_value < 0)
      continue;

    nyir_func_t candidate = {0};
    if (!nyir_func_clone(f, &candidate)) {
      nyir_scev_free(&info);
      return false;
    }
    if (peel_try_one(&candidate, loop)) {
      char err[256] = {0};
      if (nyir_verify(&candidate, err, sizeof(err))) {
        nyir_func_free(f);
        *f = candidate;
        nyir_scev_free(&info);
        return true;
      }
    }
    nyir_func_free(&candidate);
  }

  nyir_scev_free(&info);
  return true;
}

/*
 * Conservative loop predication for empty integer diamonds.
 *
 * Convert a loop-local control-only diamond selecting two already-computed
 * i64 values into wrapping arithmetic:
 *
 *   result = false_value + condition * (true_value - false_value)
 *
 * The transform is deliberately general (it is not a vector-tail guard), but
 * only fires when both arms contain no executable instruction other than their
 * branch to the same merge, both selected values dominate the condition, and
 * the condition is verifier-proven boolean.  Nothing is speculated and NyIR's
 * wrapping integer semantics make the select identity exact.
 */

typedef struct {
  int64_t target;
  int64_t local;
  int value;
} predicated_arm_t;

/*
 * Frontend conditionals commonly materialize their result through one local
 * even after mem2reg.  Accept exactly COPY*; STORE_LOCAL; BR and resolve the
 * copied value back to a definition that dominates the diamond.
 */
static bool parse_store_arm(const nyir_func_t *f, const nyir_cfg_t *cfg,
                            const int *defs, size_t block,
                            predicated_arm_t *arm) {
  if (!f || !cfg || !defs || !arm || block >= cfg->block_count)
    return false;
  *arm = (predicated_arm_t){.target = -1, .local = -1, .value = -1};
  bool saw_store = false, saw_branch = false;
  for (size_t i = cfg->block_start[block]; i < cfg->block_end[block]; ++i) {
    const nyir_inst_t *in = &f->data[i];
    if (in->op == NYIR_LABEL || in->op == NYIR_NOP || in->op == NYIR_COPY)
      continue;
    if (in->op == NYIR_STORE_LOCAL && !saw_store && !saw_branch) {
      saw_store = true;
      arm->local = in->imm;
      arm->value = in->a;
      continue;
    }
    if (in->op == NYIR_BR && saw_store && !saw_branch) {
      saw_branch = true;
      arm->target = in->imm;
      continue;
    }
    return false;
  }
  while (arm->value >= 0 && arm->value < f->next_value &&
         defs[arm->value] >= 0 && f->data[defs[arm->value]].op == NYIR_COPY)
    arm->value = f->data[defs[arm->value]].a;
  if (saw_store && !saw_branch && block + 1 < cfg->block_count) {
    arm->target = cfg->block_label[block + 1];
    saw_branch = true;
  }
  return saw_store && saw_branch;
}

static size_t skip_branch_trampoline(const nyir_func_t *f,
                                     const nyir_cfg_t *cfg, size_t block) {
  if (!f || !cfg || block >= cfg->block_count)
    return block;
  int64_t target = -1;
  for (size_t i = cfg->block_start[block]; i < cfg->block_end[block]; ++i) {
    if (f->data[i].op == NYIR_LABEL || f->data[i].op == NYIR_NOP)
      continue;
    if (f->data[i].op == NYIR_BR && target < 0) {
      target = f->data[i].imm;
      continue;
    }
    return block;
  }
  if (target < 0)
    return block;
  for (size_t b = 0; b < cfg->block_count; ++b)
    if (cfg->block_label[b] == target)
      return b;
  return block;
}

static bool bool_condition(const nyir_func_t *f, const int *defs, int value) {
  if (!f || !defs || value < 0 || value >= f->next_value || defs[value] < 0)
    return false;
  const nyir_inst_t *def = &f->data[defs[value]];
  return def->op == NYIR_CMP_I64 ||
         (def->range.has_min && def->range.has_max && def->range.min >= 0 &&
          def->range.max <= 1);
}

static bool value_dominates_block(const nyir_cfg_t *cfg, const int *defs,
                                  int value, size_t block) {
  return value >= 0 && defs[value] >= 0 &&
         nyir_cfg_dominates(cfg, cfg->inst_block[(size_t)defs[value]], block);
}

static bool replace_with_five(nyir_func_t *f, size_t at,
                              const nyir_inst_t replacement[5]) {
  if (!nir_ensure_inst_space(f, 4))
    return false;
  memmove(&f->data[at + 5], &f->data[at + 1],
          (f->len - at - 1) * sizeof(*f->data));
  memcpy(&f->data[at], replacement, 5 * sizeof(*replacement));
  f->len += 4;
  return true;
}

bool nyir_loop_predication(nyir_func_t *f) {
  if (!f || f->next_value <= 0)
    return true;
  nyir_cfg_t cfg = {0};
  int *defs = nyir_build_defs(f);
  if (!defs || !nyir_cfg_build(f, &cfg)) {
    free(defs);
    nyir_cfg_free(&cfg);
    return false;
  }

  bool *loop_member = calloc(cfg.block_count, sizeof(*loop_member));
  bool *natural = calloc(cfg.block_count, sizeof(*natural));
  if (!loop_member || !natural) {
    free(loop_member);
    free(natural);
    nyir_cfg_free(&cfg);
    free(defs);
    return false;
  }
  for (size_t latch = 0; latch < cfg.block_count; ++latch) {
    for (size_t edge = cfg.succ_offsets[latch];
         edge < cfg.succ_offsets[latch + 1]; ++edge) {
      size_t header = cfg.succ_blocks[edge];
      if (!nyir_cfg_is_backedge(&cfg, latch, header))
        continue;
      if (!nyir_cfg_natural_loop_blocks(&cfg, latch, header, natural,
                                         cfg.block_count)) {
        free(loop_member);
        free(natural);
        nyir_cfg_free(&cfg);
        free(defs);
        return false;
      }
      for (size_t block = 0; block < cfg.block_count; ++block)
        loop_member[block] = loop_member[block] || natural[block];
    }
  }

  for (size_t head = 0; head < cfg.block_count; ++head) {
    if (!loop_member[head])
      continue;
    size_t end = cfg.block_end[head];
    while (end > cfg.block_start[head] && f->data[end - 1].op == NYIR_NOP)
      --end;
    if (end == cfg.block_start[head] || f->data[end - 1].op != NYIR_BR_IF)
      continue;
    size_t br_i = end - 1;
    nyir_inst_t *br = &f->data[br_i];
    if (!bool_condition(f, defs, br->a))
      continue;

    size_t true_block = SIZE_MAX;
    for (size_t b = 0; b < cfg.block_count; ++b)
      if (cfg.block_label[b] == br->imm)
        true_block = b;
    size_t false_block = head + 1;
    if (true_block == SIZE_MAX || false_block >= cfg.block_count)
      continue;
    false_block = skip_branch_trampoline(f, &cfg, false_block);
    if (true_block == false_block)
      continue;
    predicated_arm_t true_arm, false_arm;
    if (!parse_store_arm(f, &cfg, defs, true_block, &true_arm) ||
        !parse_store_arm(f, &cfg, defs, false_block, &false_arm) ||
        true_arm.target != false_arm.target ||
        true_arm.local != false_arm.local || true_arm.local < 0 ||
        !value_dominates_block(&cfg, defs, true_arm.value, head) ||
        !value_dominates_block(&cfg, defs, false_arm.value, head))
      continue;

    int sub = f->next_value++;
    int mul = f->next_value++;
    int result = f->next_value++;
    nyir_debug_loc_t debug = br->debug;
    nyir_inst_t replacement[5] = {
        {.op=NYIR_SUB_I64,.dst=sub,.a=true_arm.value,.b=false_arm.value,
         .c=-1,.d=-1,.e=-1,.f=-1,.debug=debug},
        {.op=NYIR_MUL_I64,.dst=mul,.a=br->a,.b=sub,
         .c=-1,.d=-1,.e=-1,.f=-1,.debug=debug},
        {.op=NYIR_ADD_I64,.dst=result,.a=false_arm.value,.b=mul,
         .c=-1,.d=-1,.e=-1,.f=-1,.debug=debug},
        {.op=NYIR_STORE_LOCAL,.dst=-1,.a=result,.b=-1,.c=-1,.d=-1,
         .e=-1,.f=-1,.imm=true_arm.local,.debug=debug},
        {.op=NYIR_BR,.dst=-1,.a=-1,.b=-1,.c=-1,.d=-1,.e=-1,.f=-1,
         .imm=true_arm.target,.debug=debug}};
    if (!replace_with_five(f, br_i, replacement)) {
      free(loop_member);
      free(natural);
      nyir_cfg_free(&cfg);
      free(defs);
      return false;
    }
    nyir_refresh_metadata(f);
    free(loop_member);
    free(natural);
    nyir_cfg_free(&cfg);
    free(defs);
    return true;
  }
  free(loop_member);
  free(natural);
  nyir_cfg_free(&cfg);
  free(defs);
  return true;
}

/*
 * Loop rotation proof pass. Canonical NyIR while loops already have a rotated
 * latch; this pass removes a redundant unconditional branch immediately before
 * a header label only when fallthrough reaches the same header.
 */

bool nyir_loop_rotate(nyir_func_t *f) {
  if (!f || f->len < 2)
    return true;
  for (size_t i = 1; i < f->len; ++i) {
    if (f->data[i].op == NYIR_LABEL && f->data[i - 1].op == NYIR_BR &&
        f->data[i - 1].imm == f->data[i].imm)
      nyir_inst_discard(&f->data[i - 1]);
  }
  return true;
}

/*
 * Loop unswitching: duplicates loop bodies when a loop-invariant
 * conditional branch can be evaluated once outside the loop.
 */

/*
 * Loop Unswitching: Move loop-invariant branches out of loops.
 *
 * Pattern:
 * for (...) {
 * if (invariant) { A } else { B }
 * ...
 * }
 *
 * Becomes:
 * if (invariant) {
 * for (...) { A; ... }
 * } else {
 * for (...) { B; ... }
 * }
 *
 * This enables LICM, vectorization, and other opts on specialized
 * loop bodies. Only unswitches BR_IF with invariant conditions.
 */

typedef struct {
  int head_label;
  size_t head_idx;
  size_t body_start;
  size_t back_edge;
  size_t end_idx;
} loop_t;

/*
 * Check if a value is defined inside a loop body.
 */
static bool unswitch_value_defined_in_loop(const nyir_func_t *f, int value,
                                  const loop_t *lp) {
  for (size_t i = lp->body_start; i <= lp->back_edge; ++i) {
    if (f->data[i].dst == value)
      return true;
  }
  return false;
}

/*
 * Check if an instruction's operands are all loop-invariant.
 */
static __attribute__((unused)) bool operands_loop_invariant(const nyir_func_t *f, const nyir_inst_t *in,
                                    const loop_t *lp) {
  int inputs[] = {in->a, in->b, in->c, in->d, in->e, in->f};
  for (int k = 0; k < 6; ++k) {
    if (inputs[k] < 0)
      continue;
    if (unswitch_value_defined_in_loop(f, inputs[k], lp))
      return false;
  }
  return true;
}

bool nyir_loop_unswitch(nyir_func_t *f) {
  if (!f || f->len < 8 || f->next_value <= 0)
    return true;

  nyir_cfg_t cfg = {0};
  if (!nyir_cfg_build(f, &cfg))
    return false;

  /*
   * Find all loops (same logic as LICM).
   */
  loop_t *loops = NULL;
  size_t loop_count = 0;
  size_t loop_cap = 0;

  for (size_t i = 0; i < f->len; ++i) {
    nyir_inst_t *in = &f->data[i];
    if (in->op != NYIR_BR || in->imm < 0)
      continue;
    int target = in->imm;
    for (size_t j = i; j > 0; --j) {
      if (f->data[j - 1].op == NYIR_LABEL && f->data[j - 1].imm == target) {
        size_t latch = cfg.inst_block[i];
        size_t header = cfg.inst_block[j - 1];
        if (!nyir_cfg_is_backedge(&cfg, latch, header))
          break;
        if (loop_count == loop_cap) {
          size_t cap = loop_cap ? loop_cap * 2 : 16;
          if (cap < loop_cap || cap > SIZE_MAX / sizeof(*loops)) {
            free(loops);
            nyir_cfg_free(&cfg);
            return false;
          }
          loop_t *grown = realloc(loops, cap * sizeof(*loops));
          if (!grown) {
            free(loops);
            nyir_cfg_free(&cfg);
            return false;
          }
          loops = grown;
          loop_cap = cap;
        }
        loops[loop_count].head_label = target;
        loops[loop_count].head_idx = j - 1;
        loops[loop_count].body_start = j;
        loops[loop_count].back_edge = i;
        loops[loop_count].end_idx = i + 1;
        for (size_t k = i + 1; k < f->len; ++k) {
          if (f->data[k].op == NYIR_LABEL) {
            loops[loop_count].end_idx = k;
            break;
          }
        }
        loop_count++;
        break;
      }
    }
  }

  if (loop_count == 0) {
    free(loops);
    nyir_cfg_free(&cfg);
    return true;
  }

  /*
   * Sort loops innermost first (by body_start descending).
   */
  for (size_t i = 0; i + 1 < loop_count; ++i) {
    for (size_t j = i + 1; j < loop_count; ++j) {
      if (loops[j].body_start > loops[i].body_start) {
        loop_t tmp = loops[i];
        loops[i] = loops[j];
        loops[j] = tmp;
      }
    }
  }


  for (size_t li = 0; li < loop_count; ++li) {
    loop_t *lp = &loops[li];

    /*
     * Find BR_IF with invariant condition in the loop body.
     */
    for (size_t i = lp->body_start; i <= lp->back_edge; ++i) {
      nyir_inst_t *br_if = &f->data[i];
      if (br_if->op != NYIR_BR_IF)
        continue;
      if (br_if->a < 0)
        continue;

      /*
       * Check if condition is loop-invariant.
       */
      bool all_outside = true;
      for (size_t j = lp->body_start; j <= lp->back_edge; ++j) {
        if (f->data[j].dst == br_if->a) {
          all_outside = false;
          break;
        }
      }
      if (!all_outside)
        continue;

      /*
       * Loop cloning is required to specialize an invariant branch.  Keeping
       * one edge here would silently change the program when the condition is
       * false, so leave the CFG unchanged until cloning is implemented.
       */
      continue;
    }
  }

  free(loops);
  nyir_cfg_free(&cfg);
  return true;
}
/*
 * Canonical single-version form for loop-invariant integer predicates.
 */

static int versioning_root_copy(const nyir_func_t *f, const int *defs, int value) {
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
          int left = versioning_root_copy(f, defs, in->a);
          int right = versioning_root_copy(f, defs, in->b);
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
