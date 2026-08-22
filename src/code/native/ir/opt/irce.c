/*
 * Inductive range-check elimination driven only by SCEV-lite proofs.
 */
#include "code/native/ir/opt/loop_analysis.h"
#include "code/native/ir/opt/util.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static int strip_copies(const nyir_func_t *f, const int *defs, int value) {
  for (int depth = 0; value >= 0 && value < f->next_value && depth < 16;
       ++depth) {
    int di = defs[value];
    if (di < 0 || f->data[di].op != NYIR_COPY)
      break;
    value = f->data[di].a;
  }
  return value;
}

static bool const_i64(const nyir_func_t *f, const int *defs, int value,
                      int64_t *out) {
  value = strip_copies(f, defs, value);
  if (value < 0 || value >= f->next_value || defs[value] < 0 ||
      f->data[defs[value]].op != NYIR_CONST_I64)
    return false;
  *out = f->data[defs[value]].imm;
  return true;
}

static bool scaled_value(const nyir_func_t *f, const int *defs, int value,
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
  const nyir_inst_t *d = &f->data[defs[value]];
  int64_t c = 0;
  if (d->op == NYIR_MUL_I64) {
    if (strip_copies(f, defs, d->a) == base &&
        const_i64(f, defs, d->b, &c) && c > 0) {
      *scale = (uint64_t)c;
      return true;
    }
    if (strip_copies(f, defs, d->b) == base &&
        const_i64(f, defs, d->a, &c) && c > 0) {
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

static bool scaled_iv(const nyir_func_t *f, const int *defs, int value, int iv,
                      uint64_t *scale) {
  return scaled_value(f, defs, value, iv, scale);
}


static bool irce_insert_inst(nyir_func_t *f, size_t at, nyir_inst_t inst) {
  if (!f || at > f->len)
    return false;
  if (f->len == f->cap) {
    size_t cap = f->cap ? f->cap * 2u : 64u;
    if (cap < f->cap || cap > SIZE_MAX / sizeof(*f->data))
      return false;
    nyir_inst_t *grown = realloc(f->data, cap * sizeof(*grown));
    if (!grown)
      return false;
    f->data = grown;
    f->cap = cap;
  }
  memmove(&f->data[at + 1], &f->data[at],
          (f->len - at) * sizeof(*f->data));
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
static bool irce_hoist_one_invariant_check(nyir_func_t *f, bool *moved) {
  if (moved)
    *moved = false;
  if (!f || f->next_value <= 0)
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
  bool *in_loop = calloc(cfg.block_count, sizeof(*in_loop));
  if (!in_loop) {
    free(defs);
    nyir_scev_free(&info);
    nyir_cfg_free(&cfg);
    return false;
  }
  for (size_t li = 0; li < info.count; ++li) {
    const nyir_scev_loop_t *l = &info.loops[li];
    if (!l->trip_count_known || l->trip_count == 0 ||
        l->preheader_block >= cfg.block_count ||
        l->header_block >= cfg.block_count || l->latch_block >= cfg.block_count)
      continue;
    size_t sb = cfg.succ_offsets[l->preheader_block];
    size_t se = cfg.succ_offsets[l->preheader_block + 1];
    if (se - sb != 1 || cfg.succ_blocks[sb] != l->header_block)
      continue;
    if (!nyir_cfg_natural_loop_blocks(&cfg, l->latch_block, l->header_block,
                                     in_loop, cfg.block_count)) {
      free(in_loop);
      free(defs);
      nyir_scev_free(&info);
      nyir_cfg_free(&cfg);
      return false;
    }
    for (size_t block = 0; block < cfg.block_count; ++block) {
      if (!in_loop[block])
        continue;
      for (size_t i = cfg.block_start[block]; i < cfg.block_end[block]; ++i) {
        nyir_inst_t *check = &f->data[i];
        if (check->op != NYIR_BOUNDS_CHECK)
          continue;
        size_t check_block = cfg.inst_block[i];
        if (!nyir_cfg_dominates(&cfg, check_block, l->latch_block))
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
            if (!in_loop[succ] &&
                !nyir_cfg_dominates(&cfg, check_block, b)) {
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
          nyir_op_t last = f->data[pre_end - 1u].op;
          if (last == NYIR_BR || last == NYIR_BR_IF || last == NYIR_RET)
            at = pre_end - 1u;
        }

        int operands[] = {check->a, check->b, check->c};
        bool invariant = true;
        for (size_t k = 0; k < sizeof(operands) / sizeof(operands[0]); ++k) {
          int value = operands[k];
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
               !nyir_cfg_dominates(&cfg, def_block, l->preheader_block))) {
            invariant = false;
            break;
          }
        }
        if (!invariant)
          continue;

        nyir_inst_t hoisted = *check;
        *check = (nyir_inst_t){.op = NYIR_NOP, .dst = -1, .a = -1, .b = -1,
                               .c = -1, .d = -1, .e = -1, .f = -1};
        if (!irce_insert_inst(f, at, hoisted)) {
          *check = hoisted;
          free(in_loop);
          free(defs);
          nyir_scev_free(&info);
          nyir_cfg_free(&cfg);
          return false;
        }
        if (moved)
          *moved = true;
        free(in_loop);
        free(defs);
        nyir_scev_free(&info);
        nyir_cfg_free(&cfg);
        return true;
      }
    }
  }
  free(in_loop);
  free(defs);
  nyir_scev_free(&info);
  nyir_cfg_free(&cfg);
  return true;
}
static bool strip_const_check(nyir_func_t *f, const int *defs,
                              nyir_inst_t *check) {
  int64_t offset = 0;
  int64_t bytes = check->imm;
  if (check->b >= 0 && const_i64(f, defs, check->b, &offset) &&
      offset >= 0) {
    if (check->c >= 0 && const_i64(f, defs, check->c, &bytes)) {
      if (bytes > 0 && offset < bytes) {
        nyir_inst_discard(check);
        return true;
      }
    } else if (bytes > 0 && offset < bytes) {
      nyir_inst_discard(check);
      return true;
    }
  }
  return false;
}

static bool relational_check_safe(const nyir_func_t *f,
                                  const nyir_value_fact_t *facts,
                                  size_t at, const nyir_inst_t *check) {
  if (!f || !facts || !check || check->b < 0)
    return false;
  nyir_range_t index = {0};
  if (!nir_value_range_at(f, facts, check->b, at, &index) ||
      !index.has_min || !index.has_max || index.min < 0)
    return false;
  nyir_range_t length = {0};
  if (check->c >= 0) {
    if (!nir_value_range_at(f, facts, check->c, at, &length))
      return false;
  } else {
    length = (nyir_range_t){.has_min = true,
                            .has_max = true,
                            .min = check->imm,
                            .max = check->imm};
  }
  return length.has_min && index.max < length.min;
}


static bool irce_value_nonnegative(const nyir_func_t *f, const int *defs,
                                   const nyir_value_fact_t *facts, int value,
                                   size_t at) {
  if (!f || !defs || !facts || value < 0 || value >= f->next_value)
    return false;
  nyir_range_t r = {0};
  if (nir_value_range_at(f, facts, value, at, &r) && r.has_min && r.min >= 0)
    return true;
  int64_t c = 0;
  if (const_i64(f, defs, value, &c))
    return c >= 0;
  value = strip_copies(f, defs, value);
  if (value < 0 || value >= f->next_value || defs[value] < 0)
    return false;
  const nyir_inst_t *phi = &f->data[defs[value]];
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
    const nyir_inst_t *upd = &f->data[di];
    if (upd->op != NYIR_ADD_I64)
      return false;
    int step = -1;
    if (strip_copies(f, defs, upd->a) == value)
      step = upd->b;
    else if (strip_copies(f, defs, upd->b) == value)
      step = upd->a;
    else
      return false;
    nyir_range_t sr = {0};
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

static int irce_block_for_label(const nyir_cfg_t *cfg, int64_t label) {
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
static bool guarded_scaled_check_safe(const nyir_func_t *f,
                                      const nyir_cfg_t *cfg,
                                      const int *defs,
                                      const nyir_value_fact_t *facts,
                                      size_t check_idx,
                                      const nyir_inst_t *check) {
  if (!f || !cfg || !defs || !facts || !check || check->b < 0 ||
      check->c < 0 || check_idx >= f->len)
    return false;
  size_t check_block = cfg->inst_block[check_idx];
  if (check_block >= cfg->block_count)
    return false;
  for (size_t bi = 0; bi < f->len; ++bi) {
    const nyir_inst_t *br = &f->data[bi];
    if (br->op != NYIR_BR_IF || br->a < 0 || br->a >= f->next_value)
      continue;
    int cdi = defs[br->a];
    if (cdi < 0 || (size_t)cdi >= f->len)
      continue;
    const nyir_inst_t *cmp = &f->data[cdi];
    if (cmp->op != NYIR_CMP_I64 || cmp->cmp != NYIR_CMP_LT ||
        cmp->a < 0 || cmp->b < 0)
      continue;
    int true_block = irce_block_for_label(cfg, br->imm);
    if (true_block < 0 ||
        !nyir_cfg_dominates(cfg, (size_t)true_block, check_block))
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
static bool irce_affine_offset_range(const nyir_func_t *f, const int *defs,
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
  const nyir_inst_t *in = &f->data[di];
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
    if (c >= 0 &&
        irce_affine_offset_range(f, defs, iv_known, iv_min, iv_max, variable,
                                 depth + 1, &amin, &amax)) {
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

static void irce_elide_affine_const_checks(nyir_func_t *f,
                                           const nyir_scev_info_t *info,
                                           const int *defs) {
  if (!f || !info || !defs || f->next_value <= 0)
    return;
  size_t values = (size_t)f->next_value;
  bool *iv_known = calloc(values, sizeof(*iv_known));
  int64_t *iv_min = calloc(values, sizeof(*iv_min));
  int64_t *iv_max = calloc(values, sizeof(*iv_max));
  if (!iv_known || !iv_min || !iv_max)
    goto done;
  for (size_t i = 0; i < info->count; ++i) {
    const nyir_scev_loop_t *loop = &info->loops[i];
    if (loop->iv < 0 || (size_t)loop->iv >= values || !loop->limit_is_const ||
        loop->predicate != NYIR_CMP_LT || loop->step <= 0 || loop->init < 0 ||
        loop->limit <= loop->init)
      continue;
    iv_known[loop->iv] = true;
    iv_min[loop->iv] = loop->init;
    iv_max[loop->iv] = loop->limit - 1;
  }
  for (size_t i = 0; i < f->len; ++i) {
    nyir_inst_t *check = &f->data[i];
    if (check->op != NYIR_BOUNDS_CHECK || check->c >= 0 || check->imm <= 0)
      continue;
    int64_t min = 0, max = 0;
    if (irce_affine_offset_range(f, defs, iv_known, iv_min, iv_max, check->b,
                                 0, &min, &max) &&
        min >= 0 && max < check->imm)
      nyir_inst_discard(check);
  }
done:
  free(iv_known);
  free(iv_min);
  free(iv_max);
}

bool nyir_irce(nyir_func_t *f) {
  if (!f || f->next_value <= 0)
    return true;
  nyir_scev_info_t info = {0};
  int *defs = nyir_build_defs(f);
  nyir_value_fact_t *facts =
      (nyir_value_fact_t *)calloc((size_t)f->next_value, sizeof(*facts));
  if (!defs || !facts || !nyir_scev_analyze(f, &info) ||
      !nyir_analyze_values(f, facts, (size_t)f->next_value, NULL, 0)) {
    free(defs);
    free(facts);
    nyir_scev_free(&info);
    return false;
  }
  for (size_t i = 0; i < f->len; ++i) {
    if (f->data[i].op != NYIR_BOUNDS_CHECK)
      continue;
    if (relational_check_safe(f, facts, i, &f->data[i]))
      nyir_inst_discard(&f->data[i]);
    else
      strip_const_check(f, defs, &f->data[i]);
  }
  irce_elide_affine_const_checks(f, &info, defs);
  nyir_cfg_t cfg = {0};
  bool cfg_ok = nyir_cfg_build(f, &cfg);
  bool *in_loop = cfg_ok ? calloc(cfg.block_count, sizeof(*in_loop)) : NULL;
  if (!cfg_ok || !in_loop) {
    free(in_loop);
    nyir_cfg_free(&cfg);
    free(defs);
    free(facts);
    nyir_scev_free(&info);
    return false;
  }
  for (size_t i = 0; i < f->len; ++i) {
    nyir_inst_t *check = &f->data[i];
    if (check->op == NYIR_BOUNDS_CHECK &&
        guarded_scaled_check_safe(f, &cfg, defs, facts, i, check))
      nyir_inst_discard(check);
  }
  for (size_t li = 0; li < info.count; ++li) {
    const nyir_scev_loop_t *l = &info.loops[li];

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
    if (l->step > 0 && l->init >= 0 && l->predicate == NYIR_CMP_LT &&
        l->limit_value >= 0 &&
        nyir_cfg_natural_loop_blocks(&cfg, l->latch_block, l->header_block,
                                     in_loop, cfg.block_count)) {
      for (size_t block = 0; block < cfg.block_count; ++block) {
        if (!in_loop[block])
          continue;
        for (size_t i = cfg.block_start[block]; i < cfg.block_end[block]; ++i) {
          nyir_inst_t *check = &f->data[i];
          if (check->op != NYIR_BOUNDS_CHECK || check->c < 0)
            continue;
          uint64_t index_scale = 0, length_scale = 0;
          if (scaled_value(f, defs, check->b, l->iv, &index_scale) &&
              scaled_value(f, defs, check->c, l->limit_value, &length_scale) &&
              index_scale == length_scale)
            nyir_inst_discard(check);
        }
      }
    }

    if (!l->trip_count_known || l->trip_count == 0 || l->step <= 0 ||
        l->init < 0 ||
        !nyir_cfg_natural_loop_blocks(&cfg, l->latch_block, l->header_block,
                                     in_loop, cfg.block_count))
      continue;
    __int128 last = (__int128)l->init +
                    (__int128)(l->trip_count - 1) * l->step;
    if (last < 0 || last > INT64_MAX)
      continue;
    for (size_t block = 0; block < cfg.block_count; ++block) {
      if (!in_loop[block])
        continue;
      for (size_t i = cfg.block_start[block]; i < cfg.block_end[block]; ++i) {
        nyir_inst_t *check = &f->data[i];
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
        if (proved)
          nyir_inst_discard(check);
      }
    }
  }
  free(in_loop);
  nyir_cfg_free(&cfg);
  free(defs);
  free(facts);
  nyir_scev_free(&info);
  for (;;) {
    bool moved = false;
    if (!irce_hoist_one_invariant_check(f, &moved))
      return false;
    if (!moved)
      break;
  }
  return true;
}
