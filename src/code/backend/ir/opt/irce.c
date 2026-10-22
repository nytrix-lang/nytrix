/*
 * Inductive range-check elimination driven only by SCEV-lite proofs.
 *
 * Two-phase: (1) hoist invariant checks using SCEV-lite IV range
 * facts (iv_min/iv_max), (2) elide affine constant checks proven
 * safe by trip-count derived ranges. irce_affine_offset_range handles
 * composite indices like i*M + j.
 *
 * References:
 *  Bodik, Gupta, Sarkar — "ABCD: Eliminating Array Bounds Checks on
 *   Demand" (PLDI 2000). §3–5: inequality graph, demand-driven BCE.
 *  Rugina & Rinard — "Symbolic Bounds Analysis of Pointers, Array
 *   Indices, and Accessed Memory Regions" (PLDI 2000).
 *  Pop, Cohen, Silber — SCEV (INRIA TR, 2004). §2–3.
 */
#include "base/debug.h"
#include "code/backend/ir/opt/loop.h"
#include "code/backend/ir/opt/util.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static bool irce_affine_offset_range(const nyr_func_t *f, const int *defs,
                                     const bool *iv_known,
                                     const int64_t *iv_min,
                                     const int64_t *iv_max, int value,
                                     unsigned depth, int64_t *min_out,
                                     int64_t *max_out);
static int strip_copies(const nyr_func_t *f, const int *defs, int value) {
  for (int depth = 0; value >= 0 && value < f->next_value && depth < 16;
       ++depth) {
    int di = defs[value];
    if (di < 0 || f->data[di].op != NYIR_COPY)
      break;
    value = f->data[di].a;
  }
  return value;
}

static bool const_i64(const nyr_func_t *f, const int *defs, int value,
                      int64_t *out) {
  value = strip_copies(f, defs, value);
  if (value < 0 || value >= f->next_value || defs[value] < 0 ||
      f->data[defs[value]].op != NYIR_CONST_I64)
    return false;
  *out = f->data[defs[value]].imm;
  return true;
}

static bool scaled_value(const nyr_func_t *f, const int *defs, int value,
                         int base, uint64_t *scale) {
  if (!scale)
    return false;
  value = strip_copies(f, defs, value);
  base = strip_copies(f, defs, base);
  if (value == base) {
    *scale = 1;
    return true;
  }
  if (value < 0 || value >= f->next_value || defs[value] < 0)
    return false;
  const nyr_inst_t *d = &f->data[defs[value]];
  int64_t c = 0;
  if (d->op == NYIR_MUL_I64) {
    if (strip_copies(f, defs, d->a) == base && const_i64(f, defs, d->b, &c) &&
        c > 0) {
      *scale = (uint64_t)c;
      return true;
    }
    if (strip_copies(f, defs, d->b) == base && const_i64(f, defs, d->a, &c) &&
        c > 0) {
      *scale = (uint64_t)c;
      return true;
    }
  }
  if (d->op == NYIR_SHL_I64 && strip_copies(f, defs, d->a) == base &&
      const_i64(f, defs, d->b, &c) && c >= 0 && c < 63) {
    *scale = UINT64_C(1) << (unsigned)c;
    return true;
  }
  return false;
}

static bool scaled_iv(const nyr_func_t *f, const int *defs, int value, int iv,
                      uint64_t *scale) {
  return scaled_value(f, defs, value, iv, scale);
}

static bool irce_insert_inst(nyr_func_t *f, size_t at, nyr_inst_t inst) {
  if (!f || at > f->len)
    return false;
  if (f->len == f->cap) {
    size_t cap = f->cap ? f->cap * 2u : 64u;
    if (cap < f->cap || cap > SIZE_MAX / sizeof(*f->data))
      return false;
    nyr_inst_t *grown = realloc(f->data, cap * sizeof(*grown));
    if (!grown)
      return false;
    f->data = grown;
    f->cap = cap;
  }
  memmove(&f->data[at + 1], &f->data[at], (f->len - at) * sizeof(*f->data));
  f->data[at] = inst;
  f->len++;
  return true;
}

/*
 * Hoist one invariant bounds check to a loop preheader only when doing so is
 * trap-equivalent: the loop has a statically positive trip count, the
 * preheader has no alternate successor, the check's block dominates the
 * latch, every non-header loop exit is dominated by the check, and every
 * checked SSA operand is defined outside the loop.  One-at-a-time rewriting
 * keeps CFG/SCEV indices fresh after insertion.
 */
static bool irce_hoist_one_invariant_check(nyr_func_t *f, bool *moved) {
  if (moved)
    *moved = false;
  if (!f || f->next_value <= 0)
    return true;
  nyr_scev_info_t info = {0};
  nyr_cfg_t cfg = {0};
  int *defs = nyr_build_defs(f);
  if (!defs || !nyr_scev_analyze(f, &info) || !nyr_cfg_build(f, &cfg)) {
    free(defs);
    nyr_scev_free(&info);
    nyr_cfg_free(&cfg);
    return false;
  }
  bool *in_loop = calloc(cfg.block_count, sizeof(*in_loop));
  size_t values = (size_t)f->next_value;
  bool *iv_known = calloc(values, sizeof(*iv_known));
  int64_t *iv_min = calloc(values, sizeof(*iv_min));
  int64_t *iv_max = calloc(values, sizeof(*iv_max));
  if (!in_loop || !iv_known || !iv_min || !iv_max) {
    free(in_loop);
    free(iv_known);
    free(iv_min);
    free(iv_max);
    free(defs);
    nyr_scev_free(&info);
    nyr_cfg_free(&cfg);
    return false;
  }
  for (size_t li = 0; li < info.count; ++li) {
    const nyr_scev_loop_t *loop = &info.loops[li];
    if (loop->iv < 0 || (size_t)loop->iv >= values || !loop->init_is_const ||
        !loop->trip_count_known || loop->trip_count == 0 || loop->step <= 0 ||
        loop->init < 0)
      continue;
    iv_known[loop->iv] = true;
    iv_min[loop->iv] = loop->init;
    iv_max[loop->iv] = loop->init + (loop->trip_count - 1) * loop->step;
  }
  for (size_t li = 0; li < info.count; ++li) {
    const nyr_scev_loop_t *l = &info.loops[li];
    if (!l->trip_count_known || l->trip_count == 0 ||
        l->preheader_block >= cfg.block_count ||
        l->header_block >= cfg.block_count || l->latch_block >= cfg.block_count)
      continue;
    size_t sb = cfg.succ_offsets[l->preheader_block];
    size_t se = cfg.succ_offsets[l->preheader_block + 1];
    if (se - sb != 1 || cfg.succ_blocks[sb] != l->header_block)
      continue;
    if (!nyr_cfg_natural_loop_blocks(&cfg, l->latch_block, l->header_block,
                                      in_loop, cfg.block_count)) {
      free(in_loop);
      free(defs);
      nyr_scev_free(&info);
      nyr_cfg_free(&cfg);
      return false;
    }
    for (size_t block = 0; block < cfg.block_count; ++block) {
      if (!in_loop[block])
        continue;
      for (size_t i = cfg.block_start[block]; i < cfg.block_end[block]; ++i) {
        nyr_inst_t *check = &f->data[i];
        if (check->op != NYIR_BOUNDS_CHECK)
          continue;
        size_t check_block = cfg.inst_block[i];
        if (!nyr_cfg_dominates(&cfg, check_block, l->latch_block))
          continue;

        /*
         * A positive trip count makes the header's initial exit impossible.
         * Every other exit must occur only after the candidate check has
         * executed, otherwise moving the check to the preheader would add a
         * trap on a path that previously skipped it.
         */
        bool exits_after_check = true;
        for (size_t b = 0; b < cfg.block_count && exits_after_check; ++b) {
          if (!in_loop[b] || b == l->header_block)
            continue;
          for (size_t e = cfg.succ_offsets[b]; e < cfg.succ_offsets[b + 1];
               ++e) {
            size_t succ = cfg.succ_blocks[e];
            if (!in_loop[succ] && !nyr_cfg_dominates(&cfg, check_block, b)) {
              exits_after_check = false;
              break;
            }
          }
        }
        if (!exits_after_check)
          continue;

        size_t pre_end = cfg.block_end[l->preheader_block];
        size_t at = pre_end;
        if (pre_end > cfg.block_start[l->preheader_block]) {
          nyr_op_t last = f->data[pre_end - 1u].op;
          if (last == NYIR_BR || last == NYIR_BR_IF || last == NYIR_RET)
            at = pre_end - 1u;
        }

        int operands[] = {check->a, check->c};
        bool invariant = true;
        for (size_t k = 0; k < 2; ++k) {
          /*
           * Lowering often copies the buffer base in the loop body before
           * building an affine byte offset.  COPY has no effects and
           * preserves the operand exactly, so use its root when deciding
           * whether the checked base/length is loop invariant.
           */
          int value = strip_copies(f, defs, operands[k]);
          if (value < 0)
            continue;
          if (value >= f->next_value) {
            invariant = false;
            break;
          }
          int di = defs[value];
          if (di < 0)
            continue;
          size_t def_block = cfg.inst_block[(size_t)di];
          if (in_loop[def_block] ||
              (def_block == l->preheader_block && (size_t)di >= at) ||
              (def_block != l->preheader_block &&
               !nyr_cfg_dominates(&cfg, def_block, l->preheader_block))) {
            invariant = false;
            break;
          }
        }
        if (!invariant)
          continue;

        int value_b = strip_copies(f, defs, check->b);
        bool b_invariant = true;
        if (value_b >= 0) {
          if (value_b >= f->next_value)
            b_invariant = false;
          else {
            int di = defs[value_b];
            if (di >= 0) {
              size_t def_block = cfg.inst_block[(size_t)di];
              if (in_loop[def_block] ||
                  (def_block == l->preheader_block && (size_t)di >= at) ||
                  (def_block != l->preheader_block &&
                   !nyr_cfg_dominates(&cfg, def_block, l->preheader_block))) {
                b_invariant = false;
              }
            }
          }
        }

        int64_t max_val = 0;
        if (!b_invariant) {
          int64_t min_val = 0;
          if (!irce_affine_offset_range(f, defs, iv_known, iv_min, iv_max,
                                        check->b, 0, &min_val, &max_val) ||
              min_val < 0)
            continue;
          if (max_val < 0)
            continue;
        }

        nyr_inst_t hoisted = *check;
        hoisted.a = strip_copies(f, defs, check->a);
        hoisted.c = strip_copies(f, defs, check->c);
        if (b_invariant)
          hoisted.b = value_b;
        size_t check_idx = i;
        if (!b_invariant) {
          nyr_inst_t cinst = {.op = NYIR_CONST_I64,
                               .dst = f->next_value++,
                               .imm = max_val,
                               .a = -1,
                               .b = -1,
                               .c = -1,
                               .d = -1,
                               .e = -1,
                               .f = -1};
          if (!irce_insert_inst(f, at, cinst)) {
            free(in_loop);
            free(defs);
            nyr_scev_free(&info);
            nyr_cfg_free(&cfg);
            free(iv_known);
            free(iv_min);
            free(iv_max);
            return false;
          }
          hoisted.b = cinst.dst;
          at++;
          if (check_idx >= at)
            check_idx++;
        }

        f->data[check_idx] = (nyr_inst_t){.op = NYIR_NOP,
                                           .dst = -1,
                                           .a = -1,
                                           .b = -1,
                                           .c = -1,
                                           .d = -1,
                                           .e = -1,
                                           .f = -1};
        if (!irce_insert_inst(f, at, hoisted)) {
          free(in_loop);
          free(defs);
          nyr_scev_free(&info);
          nyr_cfg_free(&cfg);
          free(iv_known);
          free(iv_min);
          free(iv_max);
          return false;
        }
        if (moved)
          *moved = true;
        free(in_loop);
        free(defs);
        nyr_scev_free(&info);
        nyr_cfg_free(&cfg);
        free(iv_known);
        free(iv_min);
        free(iv_max);
        return true;
      }
    }
  }
  free(in_loop);
  free(iv_known);
  free(iv_min);
  free(iv_max);
  free(defs);
  nyr_scev_free(&info);
  nyr_cfg_free(&cfg);
  return true;
}
static bool strip_const_check(nyr_func_t *f, const int *defs,
                              nyr_inst_t *check) {
  /*
   * When .c carries a dynamic byte length it GOVERNS; .imm is only the
   * static capacity (lower.c emits .c = len*8, .imm = cap*8, and every
   * consumer treats c>=0 as authoritative). A constant offset proven
   * below the capacity says nothing about the dynamic length, so no
   * check with a live .c may be discarded here.
   */
  if (check->c >= 0)
    return false;
  int64_t offset = 0;
  int64_t bytes = check->imm;
  if (check->b >= 0 && const_i64(f, defs, check->b, &offset) && offset >= 0 &&
      bytes > 0 && offset < bytes) {
    nyr_inst_discard(check);
    return true;
  }
  return false;
}

static bool relational_check_safe(const nyr_func_t *f,
                                  const nyr_value_fact_t *facts, size_t at,
                                  const nyr_inst_t *check) {
  if (!f || !facts || !check || check->b < 0)
    return false;
  nyr_range_t index = {0};
  if (!nir_value_range_at(f, facts, check->b, at, &index) || !index.has_min ||
      !index.has_max || index.min < 0)
    return false;
  nyr_range_t length = {0};
  if (check->c >= 0) {
    if (!nir_value_range_at(f, facts, check->c, at, &length))
      return false;
  } else {
    length = (nyr_range_t){
        .has_min = true, .has_max = true, .min = check->imm, .max = check->imm};
  }
  return length.has_min && index.max < length.min;
}

static bool irce_value_nonnegative(const nyr_func_t *f, const int *defs,
                                   const nyr_value_fact_t *facts, int value,
                                   size_t at) {
  if (!f || !defs || !facts || value < 0 || value >= f->next_value)
    return false;
  nyr_range_t r = {0};
  if (nir_value_range_at(f, facts, value, at, &r) && r.has_min && r.min >= 0)
    return true;
  int64_t c = 0;
  if (const_i64(f, defs, value, &c))
    return c >= 0;
  value = strip_copies(f, defs, value);
  if (value < 0 || value >= f->next_value || defs[value] < 0)
    return false;
  const nyr_inst_t *phi = &f->data[defs[value]];
  if (phi->op != NYIR_PHI || phi->phi_incoming_len < 2)
    return false;
  bool saw_start = false, saw_recurrence = false;
  for (size_t i = 0; i < phi->phi_incoming_len; ++i) {
    int incoming = strip_copies(f, defs, phi->phi_incoming[i].value);
    if (incoming < 0 || incoming >= f->next_value)
      return false;
    if (const_i64(f, defs, incoming, &c)) {
      if (c < 0)
        return false;
      saw_start = true;
      continue;
    }
    int di = defs[incoming];
    if (di < 0)
      return false;
    const nyr_inst_t *upd = &f->data[di];
    if (upd->op != NYIR_ADD_I64)
      return false;
    int step = -1;
    if (strip_copies(f, defs, upd->a) == value)
      step = upd->b;
    else if (strip_copies(f, defs, upd->b) == value)
      step = upd->a;
    else
      return false;
    nyr_range_t sr = {0};
    if (step < 0 ||
        (!const_i64(f, defs, step, &c) &&
         !(nir_value_range_at(f, facts, step, at, &sr) && sr.has_min)))
      return false;
    int64_t step_min = sr.has_min ? sr.min : c;
    if (step_min < 0)
      return false;
    saw_recurrence = true;
  }
  return saw_start && saw_recurrence;
}

static int irce_block_for_label(const nyr_cfg_t *cfg, int64_t label) {
  if (!cfg)
    return -1;
  for (size_t b = 0; b < cfg->block_count; ++b)
    if (cfg->block_label[b] == label)
      return (int)b;
  return -1;
}

/*
 * Eliminate a byte-scaled bounds check directly from a dominating source
 * guard, independent of induction-step shape.  This catches loops such as
 * sieve's `j += p`: SCEV-lite deliberately rejects a non-constant step, but
 * the body is still reached only through `j < len`.  SSA dominance makes
 * that relational fact sufficient for the checked iteration.
 */
static bool guarded_scaled_check_safe(const nyr_func_t *f,
                                      const nyr_cfg_t *cfg, const int *defs,
                                      const nyr_value_fact_t *facts,
                                      size_t check_idx,
                                      const nyr_inst_t *check) {
  if (!f || !cfg || !defs || !facts || !check || check->b < 0 || check->c < 0 ||
      check_idx >= f->len)
    return false;
  size_t check_block = cfg->inst_block[check_idx];
  if (check_block >= cfg->block_count)
    return false;
  for (size_t bi = 0; bi < f->len; ++bi) {
    const nyr_inst_t *br = &f->data[bi];
    if (br->op != NYIR_BR_IF || br->a < 0 || br->a >= f->next_value)
      continue;
    int cdi = defs[br->a];
    if (cdi < 0 || (size_t)cdi >= f->len)
      continue;
    const nyr_inst_t *cmp = &f->data[cdi];
    if (cmp->op != NYIR_CMP_I64 || cmp->cmp != NYIR_CMP_LT || cmp->a < 0 ||
        cmp->b < 0)
      continue;
    int true_block = irce_block_for_label(cfg, br->imm);
    if (true_block < 0 ||
        !nyr_cfg_dominates(cfg, (size_t)true_block, check_block))
      continue;
    uint64_t index_scale = 0, length_scale = 0;
    if (scaled_value(f, defs, check->b, cmp->a, &index_scale) &&
        scaled_value(f, defs, check->c, cmp->b, &length_scale) &&
        index_scale == length_scale &&
        irce_value_nonnegative(f, defs, facts, cmp->a, check_idx))
      return true;
  }
  return false;
}

/*
 * Prove a nonnegative byte offset from loop IV intervals.  SCEV already
 * establishes each counted loop's [init, limit) domain; this small evaluator
 * composes those facts through the affine address arithmetic emitted by
 * lowering.  It deliberately rejects non-affine, negative, and overflowing
 * shapes, so it can only remove checks that are redundant on every iteration.
 */
static bool irce_affine_offset_range(const nyr_func_t *f, const int *defs,
                                     const bool *iv_known,
                                     const int64_t *iv_min,
                                     const int64_t *iv_max, int value,
                                     unsigned depth, int64_t *min_out,
                                     int64_t *max_out) {
  if (!f || !defs || !iv_known || !iv_min || !iv_max || !min_out || !max_out ||
      depth > 32)
    return false;
  value = strip_copies(f, defs, value);
  if (value < 0 || value >= f->next_value)
    return false;
  if (iv_known[value]) {
    *min_out = iv_min[value];
    *max_out = iv_max[value];
    return true;
  }
  int di = defs[value];
  if (di < 0)
    return false;
  const nyr_inst_t *in = &f->data[di];
  if (in->op == NYIR_CONST_I64 && in->imm >= 0) {
    *min_out = in->imm;
    *max_out = in->imm;
    return true;
  }
  int64_t amin = 0, amax = 0, bmin = 0, bmax = 0, c = 0;
  if (in->op == NYIR_ADD_I64 &&
      irce_affine_offset_range(f, defs, iv_known, iv_min, iv_max, in->a,
                               depth + 1, &amin, &amax) &&
      irce_affine_offset_range(f, defs, iv_known, iv_min, iv_max, in->b,
                               depth + 1, &bmin, &bmax)) {
    __int128 lo = (__int128)amin + bmin;
    __int128 hi = (__int128)amax + bmax;
    if (hi <= INT64_MAX) {
      *min_out = (int64_t)lo;
      *max_out = (int64_t)hi;
      return true;
    }
  }
  if (in->op == NYIR_SUB_I64 &&
      irce_affine_offset_range(f, defs, iv_known, iv_min, iv_max, in->a,
                               depth + 1, &amin, &amax) &&
      irce_affine_offset_range(f, defs, iv_known, iv_min, iv_max, in->b,
                               depth + 1, &bmin, &bmax) &&
      amin >= bmax) {
    *min_out = amin - bmax;
    *max_out = amax - bmin;
    return true;
  }
  if (in->op == NYIR_MUL_I64) {
    int variable = in->a;
    int constant = in->b;
    if (!const_i64(f, defs, constant, &c)) {
      variable = in->b;
      constant = in->a;
      if (!const_i64(f, defs, constant, &c))
        return false;
    }
    if (c >= 0 && irce_affine_offset_range(f, defs, iv_known, iv_min, iv_max,
                                           variable, depth + 1, &amin, &amax)) {
      __int128 lo = (__int128)amin * c;
      __int128 hi = (__int128)amax * c;
      if (hi <= INT64_MAX) {
        *min_out = (int64_t)lo;
        *max_out = (int64_t)hi;
        return true;
      }
    }
  }
  if (in->op == NYIR_SHL_I64 && const_i64(f, defs, in->b, &c) && c >= 0 &&
      c < 63 &&
      irce_affine_offset_range(f, defs, iv_known, iv_min, iv_max, in->a,
                               depth + 1, &amin, &amax) &&
      amax <= (INT64_MAX >> c)) {
    *min_out = amin << c;
    *max_out = amax << c;
    return true;
  }
  return false;
}

static void irce_elide_affine_const_checks(nyr_func_t *f,
                                           const nyr_scev_info_t *info,
                                           const int *defs,
                                           const nyr_cfg_t *cfg,
                                           bool *in_loop) {
  if (!f || !info || !defs || f->next_value <= 0)
    return;
  if (!cfg || !in_loop)
    return;
  size_t values = (size_t)f->next_value;
  bool *iv_known = calloc(values, sizeof(*iv_known));
  int64_t *iv_min = calloc(values, sizeof(*iv_min));
  int64_t *iv_max = calloc(values, sizeof(*iv_max));
  if (!iv_known || !iv_min || !iv_max)
    goto done;
  for (size_t i = 0; i < info->count; ++i) {
    const nyr_scev_loop_t *loop = &info->loops[i];
    if (loop->iv < 0 || (size_t)loop->iv >= values || !loop->init_is_const ||
        !loop->trip_count_known || loop->trip_count == 0 || loop->step <= 0 ||
        loop->init < 0 ||
        !nyr_cfg_natural_loop_blocks(cfg, loop->latch_block,
                                      loop->header_block, in_loop,
                                      cfg->block_count))
      continue;
    __int128 last =
        (__int128)loop->init + (__int128)(loop->trip_count - 1) * loop->step;
    if (last < 0 || last > INT64_MAX)
      continue;
    iv_known[loop->iv] = true;
    iv_min[loop->iv] = loop->init;
    iv_max[loop->iv] = (int64_t)last;
    for (size_t block = 0; block < cfg->block_count; ++block) {
      if (!in_loop[block])
        continue;
      for (size_t ii = cfg->block_start[block]; ii < cfg->block_end[block];
           ++ii) {
        nyr_inst_t *check = &f->data[ii];
        if (check->op != NYIR_BOUNDS_CHECK)
          continue;
        int64_t offset_min = 0, offset_max = 0, bound = 0;
        if (!irce_affine_offset_range(f, defs, iv_known, iv_min, iv_max,
                                      check->b, 0, &offset_min, &offset_max) ||
            offset_min < 0)
          continue;
        bool fixed_bound = check->c < 0 ? (bound = check->imm, bound >= 0)
                                        : const_i64(f, defs, check->c, &bound);
        if (fixed_bound && offset_max < bound)
          nyr_inst_discard(check);
      }
    }
    iv_known[loop->iv] = false;
  }
done:
  free(iv_known);
  free(iv_min);
  free(iv_max);
}

bool nyr_irce(nyr_func_t *f) {
  if (!f || f->next_value <= 0)
    return true;
  nyr_scev_info_t info = {0};
  int *defs = nyr_build_defs(f);
  nyr_value_fact_t *facts =
      (nyr_value_fact_t *)calloc((size_t)f->next_value, sizeof(*facts));
  if (!defs || !facts || !nyr_scev_analyze(f, &info) ||
      !nyr_analyze_values(f, facts, (size_t)f->next_value, NULL, 0)) {
    free(defs);
    free(facts);
    nyr_scev_free(&info);
    return false;
  }
  size_t relational_elided = 0, guarded_elided = 0;
  size_t dynamic_loop_elided = 0, static_loop_elided = 0;
  size_t checks_before = 0;
  for (size_t i = 0; i < f->len; ++i)
    checks_before += f->data[i].op == NYIR_BOUNDS_CHECK;
  for (size_t i = 0; i < f->len; ++i) {
    if (f->data[i].op != NYIR_BOUNDS_CHECK)
      continue;
    if (relational_check_safe(f, facts, i, &f->data[i])) {
      nyr_inst_discard(&f->data[i]);
      relational_elided++;
    } else {
      strip_const_check(f, defs, &f->data[i]);
    }
  }
  nyr_cfg_t cfg = {0};
  bool cfg_ok = nyr_cfg_build(f, &cfg);
  bool *in_loop = cfg_ok ? calloc(cfg.block_count, sizeof(*in_loop)) : NULL;
  if (!cfg_ok || !in_loop) {
    free(in_loop);
    nyr_cfg_free(&cfg);
    free(defs);
    free(facts);
    nyr_scev_free(&info);
    return false;
  }
  irce_elide_affine_const_checks(f, &info, defs, &cfg, in_loop);
  for (size_t i = 0; i < f->len; ++i) {
    nyr_inst_t *check = &f->data[i];
    if (check->op == NYIR_BOUNDS_CHECK &&
        guarded_scaled_check_safe(f, &cfg, defs, facts, i, check)) {
      nyr_inst_discard(check);
      guarded_elided++;
    }
  }
  for (size_t li = 0; li < info.count; ++li) {
    const nyr_scev_loop_t *l = &info.loops[li];

    /*
     * Dynamic counted-loop BCE: when the loop itself proves `iv < limit` and
     * lowering emitted the corresponding byte check
     *
     *     iv * scale < limit * scale
     *
     * the check is redundant for every executed body iteration.  This does
     * not require a compile-time trip count and is the canonical managed-list
     * indexing shape.  Require a non-negative start and positive recurrence so
     * the separate `offset >= 0` half of BOUNDS_CHECK remains proved.
     */
    if (l->init_is_const && l->step > 0 && l->init >= 0 &&
        l->predicate == NYIR_CMP_LT && l->limit_value >= 0 &&
        nyr_cfg_natural_loop_blocks(&cfg, l->latch_block, l->header_block,
                                     in_loop, cfg.block_count)) {
      for (size_t block = 0; block < cfg.block_count; ++block) {
        if (!in_loop[block])
          continue;
        for (size_t i = cfg.block_start[block]; i < cfg.block_end[block]; ++i) {
          nyr_inst_t *check = &f->data[i];
          if (check->op != NYIR_BOUNDS_CHECK || check->c < 0)
            continue;
          uint64_t index_scale = 0, length_scale = 0;
          if (scaled_value(f, defs, check->b, l->iv, &index_scale) &&
              scaled_value(f, defs, check->c, l->limit_value, &length_scale) &&
              index_scale == length_scale) {
            nyr_inst_discard(check);
            dynamic_loop_elided++;
          }
        }
      }
    }

    if (!l->init_is_const || !l->trip_count_known || l->trip_count == 0 ||
        l->step <= 0 || l->init < 0 ||
        !nyr_cfg_natural_loop_blocks(&cfg, l->latch_block, l->header_block,
                                      in_loop, cfg.block_count))
      continue;
    __int128 last = (__int128)l->init + (__int128)(l->trip_count - 1) * l->step;
    if (last < 0 || last > INT64_MAX)
      continue;
    for (size_t block = 0; block < cfg.block_count; ++block) {
      if (!in_loop[block])
        continue;
      for (size_t i = cfg.block_start[block]; i < cfg.block_end[block]; ++i) {
        nyr_inst_t *check = &f->data[i];
        if (check->op != NYIR_BOUNDS_CHECK)
          continue;
        uint64_t scale = 0;
        if (!scaled_iv(f, defs, check->b, l->iv, &scale))
          continue;
        __int128 highest = (last + 1) * scale;
        int64_t bytes = 0;
        bool proved = false;
        if (check->c >= 0 && const_i64(f, defs, check->c, &bytes))
          proved = highest <= bytes;
        else if (check->imm > 0)
          proved = highest <= check->imm;
        if (proved) {
          nyr_inst_discard(check);
          static_loop_elided++;
        }
      }
    }
  }
  size_t loops_analyzed = info.count;
  free(in_loop);
  nyr_cfg_free(&cfg);
  free(defs);
  free(facts);
  nyr_scev_free(&info);
  size_t checks_after = 0;
  for (size_t i = 0; i < f->len; ++i)
    checks_after += f->data[i].op == NYIR_BOUNDS_CHECK;
  if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
    ny_dbg_heuristic_event("IRCE",
                           "bounds checks=%zu->%zu loops=%zu relational=%zu "
                           "guarded=%zu dynamic-trip=%zu static-range=%zu "
                           "(trip-known loops use SCEV init/step/range proofs)",
                           checks_before, checks_after, loops_analyzed,
                           relational_elided, guarded_elided,
                           dynamic_loop_elided, static_loop_elided);
  else if (ny_trace_enabled("NY_TRACE_IRCE"))
    ny_trace_line("IRCE",
                  "bounds checks=%zu->%zu loops=%zu relational=%zu guarded=%zu "
                  "dynamic-trip=%zu static-range=%zu (trip-known loops use "
                  "SCEV init/step/range proofs)",
                  checks_before, checks_after, loops_analyzed,
                  relational_elided, guarded_elided, dynamic_loop_elided,
                  static_loop_elided);
  for (;;) {
    bool moved = false;
    if (!irce_hoist_one_invariant_check(f, &moved))
      return false;
    if (!moved)
      break;
  }
  return true;
}
