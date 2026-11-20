/*
 * Canonical counted-loop analysis for NyIR.
 *
 * Two-pass analysis: pass 0 discovers trip counts from primary IVs;
 * pass 1 exports secondary IVs borrowing the primary trip count.
 * Composite affine expressions (i*M + j) are handled by IRCE's
 * irce_affine_offset_range which queries SCEV-lite range facts.
 *
 * References:
 *  Pop, Cohen, Silber — "Induction Variable Analysis with Delayed
 *   Abstractions" (INRIA TR, 2004). The SCEV algorithm used by GCC.
 *  Agner Fog — Optimizing software in C++, Vol.1, Ch.12 §12.1–12.4.
 *  Cooper & Torczon — Engineering a Compiler, 2nd Ed., Ch.8 §8.5.
 */
#include "code/backend/passes/verify/loop.h"
#include "code/backend/passes/util.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

/*
 * `lo <= hi` (or `lo < hi` when `strict`) established on every path to a
 * query point.
 */
typedef struct {
  int lo;
  int hi;
  bool strict;
} scev_le_fact_t;

static bool scev_collect_le_facts(const nyr_func_t *f, const int *defs,
                                  const nyr_cfg_t *cfg, size_t at,
                                  scev_le_fact_t *facts, size_t *count,
                                  size_t cap);
static bool scev_le_reaches(int from, int to, const scev_le_fact_t *facts,
                            size_t count);
static bool scev_lt_reaches(int from, int to, const scev_le_fact_t *facts,
                            size_t count);

static int root_copy(const nyr_func_t *f, const int *defs, int value) {
  return nyr_strip_copy_chain(f, defs, value, 32);
}

static int def_index(const nyr_func_t *f, const int *defs, int value) {
  value = root_copy(f, defs, value);
  return value >= 0 && value < f->next_value ? defs[value] : -1;
}

static bool const_value(const nyr_func_t *f, const int *defs, int value,
                        int64_t *out) {
  return nyr_const_i64(f, defs, root_copy(f, defs, value), out);
}

static bool compute_trip(nyr_scev_loop_t *l) {
  if (!l->init_is_const || !l->limit_is_const || l->step == 0)
    return false;
  __int128 start = l->init, limit = l->limit, step = l->step, n = 0;
  /*
   * `i != limit` counted loops. The exit count is exact only when the
   * recurrence lands on the limit: the distance must be a whole multiple of
   * the step, in the direction of travel. A loop that misses the limit keeps
   * going until the i64 recurrence wraps, so no count may be claimed for it.
   * This mirrors LLVM's exit-count treatment of the canonical `icmp ne`
   * counted loop rather than guessing from the comparison alone.
   */
  if (l->predicate == NYIR_CMP_NE) {
    __int128 delta = limit - start;
    if (step > 0) {
      if (delta <= 0 || delta % step != 0)
        return false;
      n = delta / step;
    } else {
      if (delta >= 0 || (-delta) % (-step) != 0)
        return false;
      n = (-delta) / (-step);
    }
    __int128 ne_exit = start + n * step;
    if (n <= 0 || n > UINT64_MAX || ne_exit != limit || ne_exit < INT64_MIN ||
        ne_exit > INT64_MAX)
      return false;
    l->trip_count = (uint64_t)n;
    l->trip_count_known = true;
    return true;
  }
  if (step > 0) {
    if (l->predicate == NYIR_CMP_LT) {
      if (start >= limit)
        n = 0;
      else
        n = (limit - start + step - 1) / step;
    } else if (l->predicate == NYIR_CMP_LE) {
      if (start > limit)
        n = 0;
      else
        n = (limit - start) / step + 1;
    } else
      return false;
  } else {
    __int128 mag = -step;
    if (l->predicate == NYIR_CMP_GT) {
      if (start <= limit)
        n = 0;
      else
        n = (start - limit + mag - 1) / mag;
    } else if (l->predicate == NYIR_CMP_GE) {
      if (start < limit)
        n = 0;
      else
        n = (start - limit) / mag + 1;
    } else
      return false;
  }
  if (n < 0 || n > UINT64_MAX)
    return false;
  /*
   * NyIR i64 induction updates wrap.  A mathematical trip count is usable
   * only if the value tested after the final update is still representable;
   * otherwise wrapping can make the loop continue (or reverse direction)
   * even though the unbounded-integer recurrence would have exited.
   */
  __int128 exit_value = start + n * step;
  if (exit_value < INT64_MIN || exit_value > INT64_MAX)
    return false;
  l->trip_count = (uint64_t)n;
  l->trip_count_known = true;
  return true;
}

/*
 * Range of a runtime length-style call result. These helpers return a count
 * and report zero on failure, so the useful fact is non-negativity; the upper
 * end stays INT64_MAX because the length is dynamic. This is the general
 * call-result fact: it feeds the same range consumers as an operand range
 * without pretending to know a bound the callee does not promise.
 */
static bool scev_call_nonnegative_range(const nyr_func_t *f, const int *defs,
                                        int value, nyr_range_t *out) {
  if (!f || !defs || !out || getenv("NYTRIX_NO_SCEV_CALL_RANGES"))
    return false;
  int di = def_index(f, defs, value);
  if (di < 0 || (size_t)di >= f->len)
    return false;
  const nyr_inst_t *in = &f->data[di];
  if (in->op != NYIR_CALL || !in->symbol)
    return false;
  static const char *const nonnegative[] = {
      "rt_tbuf_len_raw", "rt_list_len_raw", "rt_list_len", "rt_len",
      "rt_len_strict",   "rt_dict_len_raw", "rt_set_len_raw"};
  for (size_t i = 0; i < sizeof(nonnegative) / sizeof(nonnegative[0]); ++i) {
    if (strcmp(in->symbol, nonnegative[i]) != 0)
      continue;
    *out = (nyr_range_t){
        .has_min = true, .has_max = true, .min = 0, .max = INT64_MAX};
    return true;
  }
  return false;
}

/*
 * Constant bounds for a counted loop whose start or limit is not a literal.
 *
 * The limit must be loop invariant: defined outside the loop in a block that
 * dominates the header. Its proven range, together with the initial value's
 * range, then bounds every value the induction variable can take even though
 * no exact trip count is known. Two facts come out:
 *
 *   body_min/body_max  values that reach the body, from the exit test;
 *   iv_min/iv_max      every observation of the phi, including the final
 *                      failing test in the exit block.
 *
 * The step must not carry the recurrence past the representable limit while
 * the test still holds; otherwise wrapping can revisit earlier values and no
 * single interval is valid. All arithmetic is 128-bit and checked.
 */
static bool scev_value_invariant_in_loop(const nyr_func_t *f,
                                         const nyr_cfg_t *cfg,
                                         const bool *in_loop, const int *defs,
                                         int value,
                                         const nyr_scev_loop_t *l);

static void compute_range_bounds(const nyr_func_t *f,
                                 const nyr_value_fact_t *facts,
                                 const nyr_cfg_t *cfg, const bool *in_loop,
                                 const int *defs, size_t at,
                                 nyr_scev_loop_t *l) {
  if (!f || !facts || !cfg || !in_loop || !defs || !l || l->step == 0 ||
      at >= f->len)
    return;
  bool up = l->step > 0;
  if (up ? (l->predicate != NYIR_CMP_LT && l->predicate != NYIR_CMP_LE)
         : (l->predicate != NYIR_CMP_GT && l->predicate != NYIR_CMP_GE))
    return;
  int lim_def = def_index(f, defs, l->limit_value);
  int init_def = def_index(f, defs, l->init_value);
  if (lim_def < 0 || init_def < 0 || (size_t)lim_def >= f->len ||
      (size_t)init_def >= f->len)
    return;
  size_t lim_block = cfg->inst_block[(size_t)lim_def];
  size_t init_block = cfg->inst_block[(size_t)init_def];
  if (lim_block >= cfg->block_count || init_block >= cfg->block_count ||
      !nyr_cfg_dominates(cfg, lim_block, l->header_block) ||
      !nyr_cfg_dominates(cfg, init_block, l->header_block))
    return;
  nyr_scev_loop_t inv = {.header_block = l->header_block};
  bool lim_inv = !in_loop[lim_block] ||
                 scev_value_invariant_in_loop(f, cfg, in_loop, defs,
                                              l->limit_value, &inv);
  bool init_inv = !in_loop[init_block] ||
                  scev_value_invariant_in_loop(f, cfg, in_loop, defs,
                                               l->init_value, &inv);
  if (!lim_inv || !init_inv)
    return;
  nyr_range_t ir = {0}, lr = {0};
  bool ir_ok = nir_value_range_at(f, facts, l->init_value, at, &ir) &&
               ir.has_min && ir.has_max;
  bool lr_ok = nir_value_range_at(f, facts, l->limit_value, at, &lr) &&
               lr.has_min && lr.has_max;
  if (!ir_ok)
    ir_ok = scev_call_nonnegative_range(f, defs, l->init_value, &ir);
  if (!lr_ok)
    lr_ok = scev_call_nonnegative_range(f, defs, l->limit_value, &lr);
  if (!ir_ok || !lr_ok)
    return;
  /*
   * A call result is only known non-negative: its INT64_MAX end gives no
   * usable interval, and recording [0, INT64_MAX) bounds would displace a
   * tighter fact a consumer has elsewhere (a tbuf length set to a constant
   * before the loop). Such a range still serves the entry test below.
   */
  if ((ir.has_max && ir.max == INT64_MAX) || (lr.has_max && lr.max == INT64_MAX))
    return;
  __int128 body_lo, body_hi, exit_edge, iv_lo, iv_hi, span;
  if (up) {
    /*
     * `lr.max - 1` must stay representable before it is used.
     */
    if (lr.max <= INT64_MIN)
      return;
    body_lo = ir.min;
    body_hi = (__int128)lr.max - (l->predicate == NYIR_CMP_LT ? 1 : 0);
    exit_edge = body_hi + l->step;
    if (exit_edge > INT64_MAX)
      return;
    iv_lo = ir.min;
    iv_hi = ir.max > exit_edge ? ir.max : exit_edge;
    span = (__int128)lr.max - ir.min;
  } else {
    if (lr.min >= INT64_MAX)
      return;
    body_lo = (__int128)lr.min + (l->predicate == NYIR_CMP_GT ? 1 : 0);
    body_hi = ir.max;
    exit_edge = body_lo + l->step;
    if (exit_edge < INT64_MIN)
      return;
    iv_lo = ir.min < exit_edge ? ir.min : exit_edge;
    iv_hi = ir.max;
    span = (__int128)ir.max - lr.min;
  }
  if (body_lo >= INT64_MIN && body_hi <= INT64_MAX && body_hi >= body_lo) {
    l->body_bounds_known = true;
    l->body_min = (int64_t)body_lo;
    l->body_max = (int64_t)body_hi;
  }
  if (iv_lo >= INT64_MIN && iv_hi <= INT64_MAX) {
    l->iv_bounds_known = true;
    l->iv_min = (int64_t)iv_lo;
    l->iv_max = (int64_t)iv_hi;
  }
  /*
   * The same interval bounds the iteration count from above.
   */
  __int128 mag = up ? (__int128)l->step : -(__int128)l->step;
  if (span > 0) {
    __int128 bound = (span + mag - 1) / mag;
    if (bound > 0 && bound <= UINT64_MAX) {
      l->trip_bound_known = true;
      l->trip_bound = (uint64_t)bound;
    }
  }
}

static uint64_t scev_magnitude_i64(int64_t value) {
  return value < 0 ? (uint64_t)(-(value + 1)) + 1u : (uint64_t)value;
}

static uint64_t scev_gcd_u64(uint64_t a, uint64_t b) {
  while (b) {
    uint64_t t = a % b;
    a = b;
    b = t;
  }
  return a;
}

/*
 * The value's definition is outside the natural loop and dominates its
 * header, so the value is loop invariant. A local load defined inside the loop
 * is also accepted when the slot is never stored to, its address is never
 * taken, and no instruction in the loop body may write memory or have unknown
 * side effects, so every iteration observes the same value.
 */
static bool scev_value_invariant_in_loop(const nyr_func_t *f,
                                         const nyr_cfg_t *cfg,
                                         const bool *in_loop, const int *defs,
                                         int value,
                                         const nyr_scev_loop_t *l) {
  int di = def_index(f, defs, value);
  if (di < 0 || (size_t)di >= f->len)
    return false;
  size_t block = cfg->inst_block[(size_t)di];
  if (block < cfg->block_count && !in_loop[block] &&
      nyr_cfg_dominates(cfg, block, l->header_block))
    return true;
  if (getenv("NYTRIX_NO_SCEV_RELOAD_INVARIANT"))
    return false;
  if (block >= cfg->block_count || !in_loop[block])
    return false;
  const nyr_inst_t *load = &f->data[(size_t)di];
  if (load->op != NYIR_LOAD_LOCAL || load->imm < 0)
    return false;
  int64_t slot = load->imm;
  for (size_t b = 0; b < cfg->block_count; ++b) {
    if (!in_loop[b])
      continue;
    size_t start = cfg->block_start[b], end = cfg->block_end[b];
    for (size_t i = start; i < end && i < f->len; ++i) {
      const nyr_inst_t *in = &f->data[i];
      if (in->op == NYIR_STORE_LOCAL && in->imm == slot)
        return false;
      if (in->op == NYIR_ADDR_LOCAL && in->imm == slot)
        return false;
      unsigned eff = nyr_effective_effects(in);
      if (eff & (NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_UNKNOWN_SIDE_EFFECT))
        return false;
    }
  }
  return true;
}

/*
 * Symbolic trip count for `for (i = init; i < limit; ++i)` (and `<=`), where
 * start and limit may both be SSA values. The count is the expression
 * `limit - init (+ 1 for <=)`; it is only recorded when the loop is provably
 * entered and the subtraction stays in i64, because outside those conditions
 * the expression is either negative (zero-trip loop) or wrapped.
 *
 * Entry comes from operand ranges when they decide the test, or from a
 * dominating branch fact (`init < limit`, or `init <= limit`). Requiring the
 * start and limit intervals also bounds `limit - init` away from overflow.
 */
static bool compute_trip_expr(const nyr_func_t *f,
                              const nyr_value_fact_t *facts,
                              const nyr_cfg_t *cfg, const bool *in_loop,
                              const int *defs, size_t at,
                              nyr_scev_loop_t *l) {
  if (!f || !facts || !cfg || !in_loop || !defs || !l || at >= f->len ||
      l->trip_count_known || l->step != 1 ||
      (l->predicate != NYIR_CMP_LT && l->predicate != NYIR_CMP_LE))
    return false;
  if (!scev_value_invariant_in_loop(f, cfg, in_loop, defs, l->init_value, l) ||
      !scev_value_invariant_in_loop(f, cfg, in_loop, defs, l->limit_value, l))
    return false;
  nyr_range_t ir = {0}, lr = {0};
  bool ir_ok = nir_value_range_at(f, facts, l->init_value, at, &ir) &&
               ir.has_min && ir.has_max;
  bool lr_ok = nir_value_range_at(f, facts, l->limit_value, at, &lr) &&
               lr.has_min && lr.has_max;
  if (!ir_ok)
    ir_ok = scev_call_nonnegative_range(f, defs, l->init_value, &ir);
  if (!lr_ok)
    lr_ok = scev_call_nonnegative_range(f, defs, l->limit_value, &lr);
  if (!ir_ok || !lr_ok)
    return false;
  bool entered = l->predicate == NYIR_CMP_LT ? ir.max < lr.min
                                              : ir.max <= lr.min;
  if (!entered) {
    scev_le_fact_t guard_facts[64];
    size_t guard_count = 0;
    if (!scev_collect_le_facts(f, defs, cfg, at, guard_facts, &guard_count,
                               64))
      return false;
    entered = l->predicate == NYIR_CMP_LT
                  ? scev_lt_reaches(root_copy(f, defs, l->init_value),
                                    root_copy(f, defs, l->limit_value),
                                    guard_facts, guard_count)
                  : scev_le_reaches(root_copy(f, defs, l->init_value),
                                    root_copy(f, defs, l->limit_value),
                                    guard_facts, guard_count);
  }
  if (!entered)
    return false;
  /*
   * The exit block observes `limit` for `<` and `limit + 1` for `<=`. The
   * latter needs an extra representable value, so it is recorded only when
   * the limit's proven range leaves room. `exit_lb` is the same expression's
   * lower bound, using the entry proof to tighten the limit below.
   */
  l->exit_value_known = true;
  l->exit_value = root_copy(f, defs, l->limit_value);
  l->exit_value_const = l->predicate == NYIR_CMP_LE ? 1 : 0;
  if (l->predicate == NYIR_CMP_LE && lr.max == INT64_MAX)
    l->exit_value_known = false;
  __int128 exit_lb_init =
      l->predicate == NYIR_CMP_LT ? (__int128)ir.min + 1 : (__int128)ir.min;
  if (exit_lb_init < lr.min)
    exit_lb_init = lr.min;
  if (exit_lb_init >= INT64_MIN && exit_lb_init <= INT64_MAX) {
    l->exit_lb_known = true;
    l->exit_lb = (int64_t)exit_lb_init;
  }
  __int128 upper =
      (__int128)lr.max - ir.min + (l->predicate == NYIR_CMP_LE ? 1 : 0);
  if (upper > INT64_MAX)
    return false;
  l->trip_expr_known = true;
  l->trip_expr_pos = root_copy(f, defs, l->limit_value);
  l->trip_expr_neg = root_copy(f, defs, l->init_value);
  l->trip_expr_const = l->predicate == NYIR_CMP_LE ? 1 : 0;
  return true;
}

/*
 * Every value an unwrapped recurrence visits is `init + k*step`, so it is a
 * multiple of gcd(|init|, |step|). Recurrences that may wrap are excluded:
 * wrapping destroys divisibility for non-power-of-two steps. The facts used
 * are an exact trip count or a range-derived IV interval, both of which
 * already prove the exit value stays representable.
 */
static void compute_multiple(nyr_scev_loop_t *l) {
  if (!l || !l->init_is_const || l->step == 0)
    return;
  if (!l->trip_count_known && !l->iv_bounds_known)
    return;
  uint64_t step = scev_magnitude_i64(l->step);
  uint64_t start = scev_magnitude_i64(l->init);
  uint64_t multiple = scev_gcd_u64(start, step);
  if (multiple < 2 || multiple > (uint64_t)INT64_MAX)
    return;
  l->iv_multiple_known = true;
  l->iv_multiple = (int64_t)multiple;
}

static bool block_for_label(const nyr_cfg_t *cfg, int64_t label,
                            size_t *out_block) {
  return nyr_cfg_block_for_label(cfg, label, out_block);
}

/*
 * Resolve a `select` to the arm selected on every path to `at`.
 *
 * This is the expression-level half of guard facts: when a dominating branch
 * tests exactly the select's condition, and one of that branch's successors
 * dominates the query, the arm taken there is the only value the select can
 * produce. The value is resolved recursively, so nested selects work, and the
 * original value is returned unchanged when no such branch exists (the select
 * then remains an opaque value, exactly as before).
 */
static int scev_resolve_select_at(const nyr_func_t *f, const int *defs,
                                  const nyr_cfg_t *cfg, size_t at, int value,
                                  unsigned depth) {
  if (!f || !defs || !cfg || at >= f->len || depth > 8 ||
      getenv("NYTRIX_NO_SCEV_SELECT_FACTS"))
    return value;
  value = root_copy(f, defs, value);
  int di = def_index(f, defs, value);
  if (di < 0 || (size_t)di >= f->len)
    return value;
  const nyr_inst_t *sel = &f->data[di];
  if (sel->op != NYIR_SELECT_I64 || sel->a < 0 || sel->b < 0 || sel->c < 0)
    return value;
  int cond = root_copy(f, defs, sel->a);
  int cdi = def_index(f, defs, cond);
  if (cdi < 0 || (size_t)cdi >= f->len || f->data[cdi].op != NYIR_CMP_I64)
    return value;
  size_t at_block = cfg->inst_block[at];
  if (at_block >= cfg->block_count)
    return value;
  for (size_t b = 0; b < cfg->block_count; ++b) {
    if (b == at_block || !nyr_cfg_dominates(cfg, b, at_block))
      continue;
    size_t end = nyr_block_last_non_nop(f, cfg, b);
    if (end <= cfg->block_start[b])
      continue;
    const nyr_inst_t *term = &f->data[end - 1];
    if (term->op != NYIR_BR_IF || root_copy(f, defs, term->a) != cond)
      continue;
    size_t taken = SIZE_MAX;
    if (!block_for_label(cfg, term->imm, &taken) ||
        taken >= cfg->block_count)
      continue;
    size_t other = SIZE_MAX;
    for (size_t e = cfg->succ_offsets[b]; e < cfg->succ_offsets[b + 1]; ++e) {
      size_t succ = cfg->succ_blocks[e];
      if (succ != taken)
        other = succ;
    }
    if (other == SIZE_MAX)
      continue;
    bool took_taken = nyr_cfg_dominates(cfg, taken, at_block);
    bool took_other = nyr_cfg_dominates(cfg, other, at_block);
    if (took_taken == took_other)
      continue;
    int arm = took_taken ? sel->b : sel->c;
    return scev_resolve_select_at(f, defs, cfg, at, arm, depth + 1);
  }
  return value;
}

static bool append_loop(nyr_scev_info_t *out, const nyr_scev_loop_t *loop) {
  if (out->count == SIZE_MAX / sizeof(*out->loops))
    return false;
  nyr_scev_loop_t *p =
      realloc(out->loops, (out->count + 1) * sizeof(*out->loops));
  if (!p)
    return false;
  out->loops = p;
  out->loops[out->count++] = *loop;
  return true;
}

bool nyr_scev_analyze(const nyr_func_t *f, nyr_scev_info_t *out) {
  if (!out)
    return false;
  memset(out, 0, sizeof(*out));
  if (!f || f->next_value <= 0)
    return true;
  nyr_cfg_t cfg = {0};
  int *defs = nyr_build_defs(f);
  if (!defs || !nyr_cfg_build(f, &cfg)) {
    free(defs);
    return false;
  }
  /*
   * Range facts for operands that are not literals. They let a counted loop
   * with a symbolic start or limit export a constant IV interval. The facts
   * are optional: when they are unavailable, analysis continues and only the
   * constant paths below apply.
   */
  nyr_value_fact_t *facts = NULL;
  if (!getenv("NYTRIX_NO_SCEV_RANGE_BOUNDS")) {
    nyr_value_fact_t *table = calloc((size_t)f->next_value, sizeof(*table));
    if (table) {
      if (nyr_analyze_values(f, table, (size_t)f->next_value, NULL, 0))
        facts = table;
      /*
       * On failure the analyzer may already have released its argument (its
       * contradiction path does), so the table is not freed here.
       */
    }
  }
  for (size_t latch = 0; latch < cfg.block_count; ++latch) {
    for (size_t e = cfg.succ_offsets[latch]; e < cfg.succ_offsets[latch + 1];
         ++e) {
      size_t header = cfg.succ_blocks[e];
      if (!nyr_cfg_is_backedge(&cfg, latch, header))
        continue;
      size_t preheader = SIZE_MAX, outside = 0;
      for (size_t p = cfg.pred_offsets[header];
           p < cfg.pred_offsets[header + 1]; ++p) {
        size_t pred = cfg.pred_blocks[p];
        if (!nyr_cfg_dominates(&cfg, header, pred)) {
          preheader = pred;
          outside++;
        }
      }
      if (outside != 1)
        continue;
      bool *in_loop = calloc(cfg.block_count, sizeof(*in_loop));
      if (!in_loop) {
        nyr_scev_free(out);
        nyr_cfg_free(&cfg);
        free(facts);
        free(defs);
        return false;
      }
      size_t header_end = nyr_block_last_non_nop(f, &cfg, header);
      const nyr_inst_t *control = header_end > cfg.block_start[header]
                                      ? &f->data[header_end - 1u]
                                      : NULL;
      size_t taken_block = SIZE_MAX;
      if (!nyr_cfg_natural_loop_blocks(&cfg, latch, header, in_loop,
                                       cfg.block_count) ||
          !control || control->op != NYIR_BR_IF ||
          !block_for_label(&cfg, control->imm, &taken_block) ||
          !in_loop[taken_block]) {
        free(in_loop);
        continue;
      }
      uint64_t trip_count = 0;
      bool trip_known = false;

      for (size_t pass = 0; pass < 2; ++pass) {
        for (size_t i = cfg.block_start[header]; i < cfg.block_end[header];
             ++i) {
          const nyr_inst_t *phi = &f->data[i];
          if (phi->op != NYIR_PHI || phi->phi_incoming_len < 2)
            continue;
          int init = -1, next = -1;
          for (size_t k = 0; k < 2; ++k) {
            int64_t label = phi->phi_incoming[k].predecessor_label;
            int v = root_copy(f, defs, phi->phi_incoming[k].value);
            if (label == cfg.block_label[preheader])
              init = v;
            if (label == cfg.block_label[latch])
              next = v;
          }
          if (init < 0 || next < 0)
            continue;
          int ni = def_index(f, defs, next);
          if (ni < 0)
            continue;
          const nyr_inst_t *inc = &f->data[ni];
          int stepv = -1;
          int64_t sign = 1;
          if (inc->op == NYIR_ADD_I64 && root_copy(f, defs, inc->a) == phi->dst)
            stepv = inc->b;
          else if (inc->op == NYIR_ADD_I64 &&
                   root_copy(f, defs, inc->b) == phi->dst)
            stepv = inc->a;
          else if (inc->op == NYIR_SUB_I64 &&
                   root_copy(f, defs, inc->a) == phi->dst)
            stepv = inc->b, sign = -1;
          else
            continue;
          int64_t step = 0, start = 0;
          if (!const_value(f, defs, stepv, &step) || step == 0)
            continue;
          bool start_is_const = const_value(f, defs, init, &start);
          if (sign < 0 && step == INT64_MIN)
            continue;
          step *= sign;

          if (pass == 0) {
            for (size_t j = cfg.block_start[header]; j < cfg.block_end[header];
                 ++j) {
              const nyr_inst_t *cmp = &f->data[j];
              if (cmp->op != NYIR_CMP_I64 || cmp->dst < 0 ||
                  root_copy(f, defs, cmp->a) != phi->dst ||
                  root_copy(f, defs, control->a) != cmp->dst)
                continue;
              nyr_scev_loop_t loop = {.header_block = header,
                                      .latch_block = latch,
                                      .preheader_block = preheader,
                                      .header_index = cfg.block_start[header],
                                      .latch_index = cfg.block_end[latch] - 1u,
                                      .header_label = cfg.block_label[header],
                                      .iv = phi->dst,
                                      .init_value = init,
                                      .next_value = next,
                                      .limit_value = cmp->b,
                                      .init = start,
                                      .init_is_const = start_is_const,
                                      .step = step,
                                      .predicate = cmp->cmp};
              loop.limit_is_const = const_value(f, defs, cmp->b, &loop.limit);
              compute_trip(&loop);
              if (loop.trip_count_known) {
                trip_known = true;
                trip_count = loop.trip_count;
              }
              break;
            }
          } else {
            bool is_primary = false;
            size_t cmp_at = SIZE_MAX;
            nyr_scev_loop_t loop = {.header_block = header,
                                    .latch_block = latch,
                                    .preheader_block = preheader,
                                    .header_index = cfg.block_start[header],
                                    .latch_index = cfg.block_end[latch] - 1u,
                                    .header_label = cfg.block_label[header],
                                    .iv = phi->dst,
                                    .init_value = init,
                                    .next_value = next,
                                    .limit_value = -1,
                                    .init = start,
                                    .init_is_const = start_is_const,
                                    .step = step,
                                    .predicate = 0};
            for (size_t j = cfg.block_start[header]; j < cfg.block_end[header];
                 ++j) {
              const nyr_inst_t *cmp = &f->data[j];
              if (cmp->op != NYIR_CMP_I64 || cmp->dst < 0 ||
                  root_copy(f, defs, cmp->a) != phi->dst ||
                  root_copy(f, defs, control->a) != cmp->dst)
                continue;
              loop.limit_value = cmp->b;
              loop.predicate = cmp->cmp;
              loop.limit_is_const = const_value(f, defs, cmp->b, &loop.limit);
              cmp_at = j;
              is_primary = true;
              break;
            }
            if (is_primary) {
              compute_trip(&loop);
              /*
               * Range facts can bound a loop whose start or limit is not a
               * literal even when no exact trip count exists. Only the
               * primary IV carries the exit test, so only it gets bounds.
               */
              if (facts && cmp_at < f->len)
                compute_range_bounds(f, facts, &cfg, in_loop, defs, cmp_at,
                                     &loop);
              if (facts && !getenv("NYTRIX_NO_SCEV_TRIP_EXPR"))
                compute_trip_expr(f, facts, &cfg, in_loop, defs, cmp_at, &loop);
              compute_multiple(&loop);
            } else if (trip_known && loop.init_is_const) {
              loop.trip_count_known = true;
              loop.trip_count = trip_count;
            }
            if (!append_loop(out, &loop)) {
              free(in_loop);
              nyr_scev_free(out);
              nyr_cfg_free(&cfg);
              free(facts);
              free(defs);
              return false;
            }
          }
        }
      }
      free(in_loop);
    }
  }
  nyr_cfg_free(&cfg);
  free(facts);
  free(defs);
  return true;
}

void nyr_scev_free(nyr_scev_info_t *info) {
  if (!info)
    return;
  free(info->loops);
  memset(info, 0, sizeof(*info));
}

static bool scev_affine_build(const nyr_func_t *f, const nyr_scev_info_t *info,
                              const int *defs, const nyr_cfg_t *cfg, int value,
                              size_t at, unsigned depth,
                              nyr_scev_affine_t *out);
static bool scev_affine_bound(const nyr_func_t *f, const nyr_scev_info_t *info,
                              const int *defs, const nyr_cfg_t *cfg, size_t at,
                              nyr_scev_affine_t *out);

/*
 * Bound a unit-step inner IV whose entry value is affine in a dominating
 * outer IV. The query must be dominated by the header's taken edge; the
 * resulting interval applies only inside the loop body, never after exit.
 */
static bool
scev_dynamic_loop_iv_bounds_at(const nyr_func_t *f, const nyr_scev_info_t *info,
                               const int *defs, const nyr_cfg_t *cfg,
                               const nyr_scev_loop_t *loop, size_t at,
                               int64_t *min_out, int64_t *max_out) {
  if (!f || !info || !defs || !cfg || !loop || loop->init_is_const ||
      !loop->limit_is_const || loop->init_value < 0 ||
      (size_t)loop->init_value >= (size_t)f->next_value ||
      loop->header_block >= cfg->block_count)
    return false;

  bool *members = calloc(cfg->block_count, sizeof(*members));
  if (!members)
    return false;
  bool in_loop = nyr_cfg_natural_loop_blocks(
      cfg, loop->latch_block, loop->header_block, members, cfg->block_count);
  size_t at_block = at < f->len ? cfg->inst_block[at] : SIZE_MAX;
  size_t header_end = nyr_block_last_non_nop(f, cfg, loop->header_block);
  size_t body_entry = SIZE_MAX;
  size_t loop_successors = 0;
  for (size_t e = cfg->succ_offsets[loop->header_block];
       e < cfg->succ_offsets[loop->header_block + 1]; ++e)
    if (members[cfg->succ_blocks[e]])
      ++loop_successors;
  bool guarded_body =
      header_end > cfg->block_start[loop->header_block] &&
      f->data[header_end - 1].op == NYIR_BR_IF &&
      block_for_label(cfg, f->data[header_end - 1].imm, &body_entry) &&
      body_entry < cfg->block_count && members[body_entry] &&
      loop_successors == 1 && body_entry != loop->header_block &&
      at_block < cfg->block_count &&
      nyr_cfg_dominates(cfg, body_entry, at_block);
  bool in_body = in_loop && guarded_body && at_block < cfg->block_count &&
                 members[at_block];
  int init_def = def_index(f, defs, loop->init_value);
  bool init_dominates =
      init_def >= 0 && (size_t)init_def < f->len &&
      cfg->inst_block[init_def] < cfg->block_count &&
      !members[cfg->inst_block[init_def]] &&
      nyr_cfg_dominates(cfg, cfg->inst_block[init_def], loop->header_block);
  free(members);
  if (!in_body || !init_dominates)
    return false;

  nyr_scev_affine_t init_range = {0};
  if (!scev_affine_build(f, info, defs, cfg, loop->init_value, (size_t)init_def,
                         0, &init_range) ||
      !scev_affine_bound(f, info, defs, cfg, (size_t)init_def, &init_range) ||
      !init_range.range.has_min || !init_range.range.has_max)
    return false;

  if (loop->step == -1 && loop->predicate == NYIR_CMP_GE &&
      loop->limit > INT64_MIN && init_range.range.max >= loop->limit) {
    *min_out = loop->limit;
    *max_out = init_range.range.max;
    return true;
  }
  if (loop->step == -1 && loop->predicate == NYIR_CMP_GT &&
      loop->limit < INT64_MAX && init_range.range.max > loop->limit) {
    *min_out = loop->limit + 1;
    *max_out = init_range.range.max;
    return true;
  }
  if (loop->step == 1 && loop->predicate == NYIR_CMP_LT &&
      loop->limit > INT64_MIN && init_range.range.min < loop->limit) {
    *min_out = init_range.range.min;
    *max_out = loop->limit - 1;
    return true;
  }
  if (loop->step == 1 && loop->predicate == NYIR_CMP_LE &&
      loop->limit < INT64_MAX && init_range.range.min <= loop->limit) {
    *min_out = init_range.range.min;
    *max_out = loop->limit;
    return true;
  }
  return false;
}

/*
 * A length query immediately following a setter on a fresh typed buffer is
 * at most the assigned nonnegative count. Keep zero in the interval: failed
 * allocation makes both runtime calls return zero. Do not fold the query or
 * carry the fact across a branch, another call, or a memory write.
 */
static bool scev_tbuf_length_upper_bound(const nyr_func_t *f, const int *defs,
                                         const nyr_cfg_t *cfg, int value,
                                         int64_t *upper) {
  value = root_copy(f, defs, value);
  int di = def_index(f, defs, value);
  if (di < 0)
    return false;
  const nyr_inst_t *query = &f->data[di];
  if (query->op != NYIR_CALL || !query->symbol ||
      strcmp(query->symbol, "rt_tbuf_len_raw") != 0)
    return false;
  int base = root_copy(f, defs, query->a);
  int base_i = def_index(f, defs, base);
  if (base_i >= 0 && f->data[base_i].op == NYIR_CALL &&
      f->data[base_i].symbol &&
      strcmp(f->data[base_i].symbol, "rt_list_set_len_raw") == 0)
    base = root_copy(f, defs, f->data[base_i].a);
  base_i = def_index(f, defs, base);
  if (base_i < 0)
    return false;
  const nyr_inst_t *alloc = &f->data[base_i];
  if (alloc->op != NYIR_CALL || !alloc->symbol ||
      (strcmp(alloc->symbol, "rt_list_new") != 0 &&
       strcmp(alloc->symbol, "rt_list_new_raw") != 0 &&
       strcmp(alloc->symbol, "rt_tbuf_new_raw") != 0))
    return false;
  size_t block = cfg->inst_block[di];
  for (size_t i = (size_t)di; i > cfg->block_start[block];) {
    const nyr_inst_t *in = &f->data[--i];
    if (in->op == NYIR_CALL) {
      return in->symbol && strcmp(in->symbol, "rt_list_set_len_raw") == 0 &&
             root_copy(f, defs, in->a) == base &&
             const_value(f, defs, in->b, upper) && *upper >= 0;
    }
    if (nyr_effective_effects(in) & ~(NYIR_EFFECT_READ_LOCAL))
      return false;
  }
  return false;
}

static bool scev_loop_iv_bounds_at(const nyr_func_t *f,
                                   const nyr_scev_info_t *info, const int *defs,
                                   const nyr_cfg_t *cfg, int value, size_t at,
                                   int64_t *min_out, int64_t *max_out) {
  if (!f || !info || !defs || !cfg || !min_out || !max_out || at >= f->len)
    return false;
  size_t at_block = cfg->inst_block[at];
  for (size_t i = 0; i < info->count; ++i) {
    const nyr_scev_loop_t *loop = &info->loops[i];
    if (loop->iv != value)
      continue;
    /*
     * Range-fact bounds describe values that passed the exit test, so they
     * apply on body paths only. The whole-value interval stays in the loop
     * record for the phi's range fact.
     */
    if (loop->body_bounds_known) {
      bool *members = calloc(cfg->block_count, sizeof(*members));
      if (!members)
        return false;
      bool in_body = nyr_cfg_natural_loop_blocks(
                         cfg, loop->latch_block, loop->header_block, members,
                         cfg->block_count) &&
                     at_block < cfg->block_count && members[at_block];
      free(members);
      if (in_body) {
        *min_out = loop->body_min;
        *max_out = loop->body_max;
        return true;
      }
    }
    if (!loop->init_is_const) {
      if (scev_dynamic_loop_iv_bounds_at(f, info, defs, cfg, loop, at, min_out,
                                         max_out))
        return true;
      continue;
    }
    if (!loop->trip_count_known) {
      /*
       * A unit-step signed `<` loop cannot wrap before it exits, for any
       * runtime limit: if it enters, each increment moves toward a limit no
       * greater than INT64_MAX and the final body value is at most
       * INT64_MAX-1.  The symmetric `>` / -1 case has the same property.
       * Export only the in-loop interval; an unknown trip count gives us no
       * exhaustion-value fact after the loop.
       */
      bool unit_monotone =
          (loop->step == 1 && loop->predicate == NYIR_CMP_LT &&
           loop->init < INT64_MAX) ||
          (loop->step == -1 && loop->predicate == NYIR_CMP_GT &&
           loop->init > INT64_MIN);
      if (!unit_monotone)
        continue;
      bool *in_loop = calloc(cfg->block_count, sizeof(*in_loop));
      if (!in_loop)
        return false;
      bool in_body = nyr_cfg_natural_loop_blocks(cfg, loop->latch_block,
                                                 loop->header_block, in_loop,
                                                 cfg->block_count) &&
                     at_block < cfg->block_count && in_loop[at_block];
      free(in_loop);
      if (!in_body)
        continue;
      if (loop->step > 0) {
        *min_out = loop->init;
        *max_out = INT64_MAX - 1;
        int64_t upper = 0;
        size_t end = nyr_block_last_non_nop(f, cfg, loop->header_block);
        size_t body = SIZE_MAX;
        if (end > cfg->block_start[loop->header_block] &&
            f->data[end - 1].op == NYIR_BR_IF &&
            block_for_label(cfg, f->data[end - 1].imm, &body) &&
            body != loop->header_block &&
            nyr_cfg_dominates(cfg, body, at_block) &&
            scev_tbuf_length_upper_bound(f, defs, cfg, loop->limit_value,
                                         &upper) &&
            upper > loop->init)
          *max_out = upper - 1;
      } else {
        *min_out = INT64_MIN + 1;
        *max_out = loop->init;
      }
      return true;
    }
    if (loop->trip_count == 0)
      continue;
    __int128 last =
        (__int128)loop->init + (__int128)(loop->trip_count - 1) * loop->step;
    __int128 exit = last + loop->step;
    if (last < INT64_MIN || last > INT64_MAX || exit < INT64_MIN ||
        exit > INT64_MAX)
      continue;
    bool *in_loop = calloc(cfg->block_count, sizeof(*in_loop));
    if (!in_loop)
      return false;
    bool in_body =
        nyr_cfg_natural_loop_blocks(cfg, loop->latch_block, loop->header_block,
                                    in_loop, cfg->block_count) &&
        at_block < cfg->block_count && in_loop[at_block];
    if (in_body) {
      *min_out = loop->step > 0 ? loop->init : (int64_t)last;
      *max_out = loop->step > 0 ? (int64_t)last : loop->init;
      free(in_loop);
      return true;
    }
    /*
     * The exhaustion-value singleton is only valid on SINGLE-EXIT loops:
     * a mid-body break (or any second exit) reaches the exit block with
     * an intermediate IV value, and downstream bounds-check elimination
     * would drop a live guard on that path.
     */
    bool single_exit = true;
    size_t sole_exit = SIZE_MAX;
    for (size_t b = 0; b < cfg->block_count && single_exit; ++b) {
      if (!in_loop[b])
        continue;
      for (size_t e = cfg->succ_offsets[b]; e < cfg->succ_offsets[b + 1]; ++e) {
        size_t succ = cfg->succ_blocks[e];
        if (in_loop[succ])
          continue;
        if (sole_exit == SIZE_MAX)
          sole_exit = succ;
        else if (sole_exit != succ) {
          single_exit = false;
          break;
        }
      }
    }
    free(in_loop);
    if (single_exit && sole_exit != SIZE_MAX &&
        nyr_cfg_dominates(cfg, sole_exit, at_block)) {
      *min_out = (int64_t)exit;
      *max_out = (int64_t)exit;
      return true;
    }
  }
  return false;
}

static void scev_affine_init(nyr_scev_affine_t *out) {
  *out = (nyr_scev_affine_t){.iv = {-1, -1}};
}

static bool scev_affine_add_term(nyr_scev_affine_t *out, int iv,
                                 int64_t coefficient) {
  if (!out)
    return false;
  for (size_t i = 0; i < 2; ++i) {
    if (out->iv[i] != iv)
      continue;
    __int128 sum = (__int128)out->coefficient[i] + coefficient;
    if (sum < INT64_MIN || sum > INT64_MAX)
      return false;
    out->coefficient[i] = (int64_t)sum;
    return true;
  }
  for (size_t i = 0; i < 2; ++i) {
    if (out->iv[i] >= 0)
      continue;
    out->iv[i] = iv;
    out->coefficient[i] = coefficient;
    return true;
  }
  return false;
}

static bool scev_affine_combine(nyr_scev_affine_t *out,
                                const nyr_scev_affine_t *a,
                                const nyr_scev_affine_t *b, int sign) {
  if (!out || !a || !b || (sign != 1 && sign != -1))
    return false;
  scev_affine_init(out);
  __int128 constant = (__int128)a->constant + sign * (__int128)b->constant;
  if (constant < INT64_MIN || constant > INT64_MAX)
    return false;
  out->constant = (int64_t)constant;
  for (size_t i = 0; i < 2; ++i) {
    if (a->iv[i] >= 0 &&
        !scev_affine_add_term(out, a->iv[i], a->coefficient[i]))
      return false;
    if (b->iv[i] >= 0) {
      __int128 coefficient = sign * (__int128)b->coefficient[i];
      if (coefficient < INT64_MIN || coefficient > INT64_MAX ||
          !scev_affine_add_term(out, b->iv[i], (int64_t)coefficient))
        return false;
    }
  }
  return true;
}

static bool scev_affine_scale(nyr_scev_affine_t *out,
                              const nyr_scev_affine_t *in, int64_t scale) {
  if (!out || !in)
    return false;
  scev_affine_init(out);
  __int128 constant = (__int128)in->constant * scale;
  if (constant < INT64_MIN || constant > INT64_MAX)
    return false;
  out->constant = (int64_t)constant;
  for (size_t i = 0; i < 2; ++i) {
    if (in->iv[i] < 0)
      continue;
    __int128 coefficient = (__int128)in->coefficient[i] * scale;
    if (coefficient < INT64_MIN || coefficient > INT64_MAX ||
        !scev_affine_add_term(out, in->iv[i], (int64_t)coefficient))
      return false;
  }
  return true;
}

static bool scev_affine_build(const nyr_func_t *f, const nyr_scev_info_t *info,
                              const int *defs, const nyr_cfg_t *cfg, int value,
                              size_t at, unsigned depth,
                              nyr_scev_affine_t *out) {
  if (!f || !info || !defs || !cfg || !out || depth > 32)
    return false;
  value = root_copy(f, defs, value);
  if (value < 0 || value >= f->next_value)
    return false;
  int64_t lo = 0, hi = 0;
  if (scev_loop_iv_bounds_at(f, info, defs, cfg, value, at, &lo, &hi)) {
    scev_affine_init(out);
    return scev_affine_add_term(out, value, 1);
  }
  /*
   * Bounds may be unavailable while the structural shape is still usable: a
   * symbolic guard comparison must see the IV as a term even when no constant
   * interval exists. Term-only forms fail in scev_affine_bound, so range
   * consumers are unaffected.
   */
  for (size_t k = 0; k < info->count; ++k) {
    if (info->loops[k].iv != value)
      continue;
    scev_affine_init(out);
    return scev_affine_add_term(out, value, 1);
  }
  int di = defs[value];
  if (di < 0 || (size_t)di >= f->len)
    return false;
  const nyr_inst_t *in = &f->data[di];
  if (in->op == NYIR_CONST_I64) {
    scev_affine_init(out);
    out->constant = in->imm;
    return true;
  }
  nyr_scev_affine_t a = {0}, b = {0};
  if (in->op == NYIR_ADD_I64 &&
      scev_affine_build(f, info, defs, cfg, in->a, at, depth + 1, &a) &&
      scev_affine_build(f, info, defs, cfg, in->b, at, depth + 1, &b))
    return scev_affine_combine(out, &a, &b, 1);
  if (in->op == NYIR_SUB_I64 &&
      scev_affine_build(f, info, defs, cfg, in->a, at, depth + 1, &a) &&
      scev_affine_build(f, info, defs, cfg, in->b, at, depth + 1, &b))
    return scev_affine_combine(out, &a, &b, -1);
  if (in->op == NYIR_SHL_I64) {
    int64_t shift = 0;
    if (!const_value(f, defs, in->b, &shift) || shift < 0 || shift >= 63)
      return false;
    int64_t scale = INT64_C(1) << (unsigned)shift;
    return scev_affine_build(f, info, defs, cfg, in->a, at, depth + 1, &a) &&
           scev_affine_scale(out, &a, scale);
  }
  if (in->op != NYIR_MUL_I64)
    return false;
  int variable = in->a;
  int64_t scale = 0;
  if (!const_value(f, defs, in->b, &scale)) {
    variable = in->b;
    if (!const_value(f, defs, in->a, &scale))
      return false;
  }
  return scev_affine_build(f, info, defs, cfg, variable, at, depth + 1, &a) &&
         scev_affine_scale(out, &a, scale);
}

static bool scev_affine_bound(const nyr_func_t *f, const nyr_scev_info_t *info,
                              const int *defs, const nyr_cfg_t *cfg, size_t at,
                              nyr_scev_affine_t *out) {
  if (!f || !info || !defs || !cfg || !out)
    return false;
  __int128 lo = out->constant, hi = out->constant;
  for (size_t i = 0; i < 2; ++i) {
    if (out->iv[i] < 0 || out->coefficient[i] == 0)
      continue;
    int64_t iv_min = 0, iv_max = 0;
    if (!scev_loop_iv_bounds_at(f, info, defs, cfg, out->iv[i], at, &iv_min,
                                &iv_max))
      return false;
    __int128 first = (__int128)out->coefficient[i] * iv_min;
    __int128 last = (__int128)out->coefficient[i] * iv_max;
    if (first < last) {
      lo += first;
      hi += last;
    } else {
      lo += last;
      hi += first;
    }
  }
  if (lo < INT64_MIN || lo > INT64_MAX || hi < INT64_MIN || hi > INT64_MAX)
    return false;
  out->range = (nyr_range_t){
      .has_min = true, .has_max = true, .min = (int64_t)lo, .max = (int64_t)hi};
  return true;
}

bool nyr_scev_affine_at_cfg(const nyr_func_t *f, const nyr_scev_info_t *info,
                            const int *defs, const nyr_cfg_t *cfg, int value,
                            size_t at, nyr_scev_affine_t *out) {
  if (!f || !info || !defs || !cfg || !out || at >= f->len)
    return false;
  return scev_affine_build(f, info, defs, cfg, value, at, 0, out) &&
         scev_affine_bound(f, info, defs, cfg, at, out);
}

bool nyr_scev_affine_at(const nyr_func_t *f, const nyr_scev_info_t *info,
                        const int *defs, int value, size_t at,
                        nyr_scev_affine_t *out) {
  if (!f || !info || !defs || !out || at >= f->len)
    return false;
  nyr_cfg_t cfg = {0};
  if (!nyr_cfg_build(f, &cfg))
    return false;
  bool ok = nyr_scev_affine_at_cfg(f, info, defs, &cfg, value, at, out);
  nyr_cfg_free(&cfg);
  return ok;
}

/*
 * Split `value` into `base * scale` for a positive constant scale, recognizing
 * both `x * c` and `x << s`. Returns false when the value is not a scaled
 * form, leaving the caller to treat it as scale 1.
 */
static bool scev_scale_of_value(const nyr_func_t *f, const int *defs, int value,
                                int *base, int64_t *scale) {
  if (!f || !defs || !base || !scale)
    return false;
  value = root_copy(f, defs, value);
  int di = def_index(f, defs, value);
  if (di < 0 || (size_t)di >= f->len)
    return false;
  const nyr_inst_t *in = &f->data[di];
  if (in->op == NYIR_MUL_I64) {
    int64_t c = 0;
    if (const_value(f, defs, in->b, &c) && c > 0) {
      *base = in->a;
      *scale = c;
      return true;
    }
    if (const_value(f, defs, in->a, &c) && c > 0) {
      *base = in->b;
      *scale = c;
      return true;
    }
    return false;
  }
  if (in->op == NYIR_SHL_I64) {
    int64_t s = 0;
    if (!const_value(f, defs, in->b, &s) || s <= 0 || s >= 63)
      return false;
    *base = in->a;
    *scale = (int64_t)(UINT64_C(1) << (unsigned)s);
    return true;
  }
  return false;
}

static bool scev_tbuf_payload(const nyr_func_t *f, const int *defs, int value,
                              bool *active, unsigned depth) {
  if (!f || !defs || !active || depth > 32)
    return false;
  value = root_copy(f, defs, value);
  if (value < 0 || value >= f->next_value)
    return false;
  if (active[value])
    return true; /* A loop-carried append preserves tbuf provenance. */
  int di = defs[value];
  if (di < 0 || (size_t)di >= f->len)
    return false;
  const nyr_inst_t *in = &f->data[di];
  if (in->op == NYIR_ALLOCA || in->op == NYIR_ADDR_LOCAL ||
      in->op == NYIR_ADDR_SYMBOL)
    return true;
  if (in->op == NYIR_CALL && in->symbol) {
    if (strcmp(in->symbol, "rt_tbuf_new_raw") == 0 ||
        strcmp(in->symbol, "rt_list_new") == 0 ||
        strcmp(in->symbol, "rt_list_new_raw") == 0 ||
        strcmp(in->symbol, "malloc") == 0 ||
        strcmp(in->symbol, "calloc") == 0 ||
        strcmp(in->symbol, "rt_alloc") == 0 ||
        strcmp(in->symbol, "f64buf_new") == 0 ||
        strcmp(in->symbol, "i64buf_new") == 0 ||
        strcmp(in->symbol, "f32buf_new") == 0)
      return true;
    if (strcmp(in->symbol, "rt_tbuf_append_raw") == 0 ||
        strcmp(in->symbol, "rt_tbuf_append_i64_raw") == 0)
      return scev_tbuf_payload(f, defs, in->a, active, depth + 1);
    /*
     * `rt_list_set_len_raw` updates the tbuf header and returns that same
     * payload pointer.  Preserve the allocation provenance across this
     * list-specific wrapper so affine loop bounds can prove indexed accesses
     * without discarding the normal dynamic-length check when no range proof
     * exists.
     */
    if (strcmp(in->symbol, "rt_list_set_len_raw") == 0)
      return scev_tbuf_payload(f, defs, in->a, active, depth + 1);
  }
  if (in->op == NYIR_COPY && in->a >= 0)
    return scev_tbuf_payload(f, defs, in->a, active, depth + 1);
  if (in->op != NYIR_PHI || in->phi_incoming_len == 0)
    return false;
  active[value] = true;
  bool ok = true;
  for (size_t i = 0; i < in->phi_incoming_len; ++i) {
    if (!scev_tbuf_payload(f, defs, in->phi_incoming[i].value, active,
                           depth + 1)) {
      ok = false;
      break;
    }
  }
  active[value] = false;
  return ok;
}

static nyr_cmp_t scev_cmp_invert(nyr_cmp_t cmp) {
  switch (cmp) {
  case NYIR_CMP_LT:
    return NYIR_CMP_GE;
  case NYIR_CMP_LE:
    return NYIR_CMP_GT;
  case NYIR_CMP_GT:
    return NYIR_CMP_LE;
  case NYIR_CMP_GE:
    return NYIR_CMP_LT;
  case NYIR_CMP_EQ:
    return NYIR_CMP_NE;
  case NYIR_CMP_NE:
    return NYIR_CMP_EQ;
  }
  return NYIR_CMP_NE;
}

/*
 * Append the ordering facts implied by one comparison predicate.
 */
static bool scev_push_order_facts(int x, int y, nyr_cmp_t pred,
                                  scev_le_fact_t *facts, size_t *count,
                                  size_t cap) {
  bool le_xy = false, le_yx = false, strict = false;
  switch (pred) {
  case NYIR_CMP_LT:
    le_xy = true;
    strict = true;
    break;
  case NYIR_CMP_LE:
    le_xy = true;
    break;
  case NYIR_CMP_GT:
    le_yx = true;
    strict = true;
    break;
  case NYIR_CMP_GE:
    le_yx = true;
    break;
  case NYIR_CMP_EQ:
    le_xy = le_yx = true;
    break;
  case NYIR_CMP_NE:
    break;
  }
  if ((le_xy && *count >= cap) || (le_yx && *count + 1 >= cap))
    return false;
  if (le_xy)
    facts[(*count)++] = (scev_le_fact_t){.lo = x, .hi = y, .strict = strict};
  if (le_yx)
    facts[(*count)++] = (scev_le_fact_t){.lo = y, .hi = x, .strict = strict};
  return true;
}

/*
 * Collect `lo <= hi` relations that hold on every path to `at`, from branches
 * that dominate the query point. A branch is usable only when exactly one of
 * its successors dominates the query: then every path reaching the query took
 * that edge, so the edge's comparison (or its negation) is a fact there. A
 * merge point leaves both successors reachable, and the branch is skipped.
 */
static bool scev_collect_le_facts(const nyr_func_t *f, const int *defs,
                                  const nyr_cfg_t *cfg, size_t at,
                                  scev_le_fact_t *facts, size_t *count,
                                  size_t cap) {
  if (!f || !defs || !cfg || !facts || !count || at >= f->len)
    return false;
  size_t at_block = cfg->inst_block[at];
  if (at_block >= cfg->block_count)
    return false;
  for (size_t b = 0; b < cfg->block_count; ++b) {
    /*
     * The guard must be on the way to the query, not the query's own block.
     */
    if (b == at_block || !nyr_cfg_dominates(cfg, b, at_block))
      continue;
    size_t end = nyr_block_last_non_nop(f, cfg, b);
    if (end <= cfg->block_start[b])
      continue;
    const nyr_inst_t *term = &f->data[end - 1];
    if (term->op != NYIR_BR_IF)
      continue;
    int def = def_index(f, defs, term->a);
    if (def < 0)
      continue;
    const nyr_inst_t *cmp = &f->data[def];
    if (cmp->op != NYIR_CMP_I64 || cmp->a < 0 || cmp->b < 0)
      continue;
    size_t taken = SIZE_MAX;
    if (!block_for_label(cfg, term->imm, &taken) ||
        taken >= cfg->block_count)
      continue;
    size_t other = SIZE_MAX;
    for (size_t e = cfg->succ_offsets[b]; e < cfg->succ_offsets[b + 1]; ++e) {
      size_t succ = cfg->succ_blocks[e];
      if (succ != taken)
        other = succ;
    }
    if (other == SIZE_MAX)
      continue;
    bool took_taken = nyr_cfg_dominates(cfg, taken, at_block);
    bool took_other = nyr_cfg_dominates(cfg, other, at_block);
    if (took_taken == took_other)
      continue;
    nyr_cmp_t pred = took_taken ? cmp->cmp : scev_cmp_invert(cmp->cmp);
    int x = scev_resolve_select_at(f, defs, cfg, at,
                                   root_copy(f, defs, cmp->a), 0);
    int y = scev_resolve_select_at(f, defs, cfg, at,
                                   root_copy(f, defs, cmp->b), 0);
    scev_push_order_facts(x, y, pred, facts, count, cap);
  }
  /*
   * Assume-marked comparisons are facts at this point without a CFG edge: the
   * front end (or a lowering stage) promises the compare is true. The scan is
   * instruction-level because the assume need not be a block terminator; it
   * must still dominate the query, and in the same block it must precede it.
   */
  bool allow_assume = !getenv("NYTRIX_NO_SCEV_ASSUME_FACTS");
  for (size_t j = 0; j < f->len && j <= at && allow_assume; ++j) {
    const nyr_inst_t *cmp = &f->data[j];
    if (cmp->op != NYIR_CMP_I64 || !(cmp->flags & NYIR_INST_F_ASSUME) ||
        cmp->a < 0 || cmp->b < 0)
      continue;
    size_t cb = cfg->inst_block[j];
    if (cb >= cfg->block_count)
      continue;
    if (cb != at_block) {
      if (!nyr_cfg_dominates(cfg, cb, at_block))
        continue;
    } else if (j > at) {
      continue;
    }
    int x = scev_resolve_select_at(f, defs, cfg, at,
                                   root_copy(f, defs, cmp->a), 0);
    int y = scev_resolve_select_at(f, defs, cfg, at,
                                   root_copy(f, defs, cmp->b), 0);
    scev_push_order_facts(x, y, cmp->cmp, facts, count, cap);
  }
  return *count > 0;
}

/*
 * Transitive closure of the collected `<=` facts, bounded by the fact count.
 * Transitive closure that reports whether some `from <= to` chain contains a
 * strict edge, i.e. proves `from < to`.
 */
static bool scev_lt_reaches(int from, int to, const scev_le_fact_t *facts,
                            size_t count) {
  if (from == to || !facts || !count)
    return false;
  int nodes[64];
  bool strict[64];
  size_t node_count = 0;
  nodes[node_count] = from;
  strict[node_count++] = false;
  for (size_t iter = 0; iter <= count; ++iter) {
    bool added = false;
    for (size_t i = 0; i < count; ++i) {
      size_t lo = node_count, hi = node_count;
      for (size_t k = 0; k < node_count; ++k) {
        if (nodes[k] == facts[i].lo)
          lo = k;
        if (nodes[k] == facts[i].hi)
          hi = k;
      }
      if (lo == node_count)
        continue;
      bool want_strict = facts[i].strict || strict[lo];
      if (hi < node_count) {
        if (want_strict && !strict[hi]) {
          strict[hi] = true;
          added = true;
          if (nodes[hi] == to)
            return true;
        }
        continue;
      }
      if (node_count == 64)
        return false;
      nodes[node_count] = facts[i].hi;
      strict[node_count] = want_strict;
      added = true;
      if (facts[i].hi == to && want_strict)
        return true;
      ++node_count;
    }
    if (!added)
      break;
  }
  return false;
}

static bool scev_le_reaches(int from, int to, const scev_le_fact_t *facts,
                            size_t count) {
  if (from == to)
    return true;
  if (!facts || !count)
    return false;
  int reached[64];
  size_t reached_n = 0;
  reached[reached_n++] = from;
  for (size_t iter = 0; iter <= count; ++iter) {
    bool added = false;
    for (size_t i = 0; i < count; ++i) {
      bool have_lo = false, have_hi = false;
      for (size_t k = 0; k < reached_n; ++k) {
        have_lo |= reached[k] == facts[i].lo;
        have_hi |= reached[k] == facts[i].hi;
      }
      if (!have_lo || have_hi)
        continue;
      if (reached_n == 64)
        return false;
      reached[reached_n++] = facts[i].hi;
      added = true;
      if (facts[i].hi == to)
        return true;
    }
    if (!added)
      break;
  }
  return false;
}

/*
 * Loop-guard proof for a dynamic length.
 *
 * In `for (i = start; i < n; ++i) { check(buf, i * S, n * S); }` the exit test
 * already implies the check: the body runs only while `i < n`, and both sides
 * are scaled by the same positive factor, so `S*i + d < S*n` for `d <= 0`
 * (`d <= -1` for `<=`, where the body may run at `i == n`). No constant bound
 * on `n` is needed, so this covers dynamic-length buffers that range facts
 * cannot bound.
 *
 * The length may also come through a chain of dominating guards: when the loop
 * limit is `n` and an earlier branch proved `n <= cap` on this path, a check
 * against `cap` is equally redundant (`i < n <= cap`). Those relations are
 * collected per query point and closed transitively, which is the predicated
 * form of the loop guard.
 */
static bool scev_guard_proves_offset(const nyr_func_t *f,
                                     const nyr_scev_info_t *info,
                                     const int *defs, const nyr_cfg_t *cfg,
                                     size_t at, const nyr_inst_t *check) {
  if (!f || !info || !defs || !cfg || !check || at >= f->len)
    return false;
  nyr_scev_affine_t offset = {0};
  if (!scev_affine_build(f, info, defs, cfg, check->b, at, 0, &offset))
    return false;
  if (offset.iv[1] >= 0 || offset.iv[0] < 0 || offset.coefficient[0] <= 0)
    return false;
  int iv = offset.iv[0];
  int64_t scale = offset.coefficient[0];
  const nyr_scev_loop_t *loop = NULL;
  for (size_t i = 0; i < info->count; ++i) {
    if (info->loops[i].iv != iv)
      continue;
    loop = &info->loops[i];
    break;
  }
  if (!loop || loop->step <= 0 || !loop->init_is_const)
    return false;
  if (loop->predicate != NYIR_CMP_LT && loop->predicate != NYIR_CMP_LE)
    return false;
  if (loop->predicate == NYIR_CMP_LT ? offset.constant > 0
                                     : offset.constant >= 0)
    return false;
  __int128 lowest = (__int128)scale * loop->init + offset.constant;
  if (lowest < 0)
    return false;
  int base = check->c;
  int64_t length_scale = 1;
  int scaled_base = -1;
  if (scev_scale_of_value(f, defs, check->c, &scaled_base, &length_scale))
    base = scaled_base;
  if (length_scale != scale)
    return false;
  int length_root = scev_resolve_select_at(f, defs, cfg, at, base, 0);
  int limit_root = scev_resolve_select_at(f, defs, cfg, at,
                                          loop->limit_value, 0);
  bool ordered = length_root == limit_root;
  if (!ordered) {
    /*
     * Predicated guard: the limit is at most the check's length here.
     */
    scev_le_fact_t facts[64];
    size_t fact_count = 0;
    ordered = scev_collect_le_facts(f, defs, cfg, at, facts, &fact_count, 64) &&
              scev_le_reaches(limit_root, length_root, facts, fact_count);
  }
  if (!ordered)
    return false;
  bool *members = calloc(cfg->block_count, sizeof(*members));
  if (!members)
    return false;
  size_t header_end = nyr_block_last_non_nop(f, cfg, loop->header_block);
  size_t body_entry = SIZE_MAX;
  bool in_loop = nyr_cfg_natural_loop_blocks(cfg, loop->latch_block,
                                             loop->header_block, members,
                                             cfg->block_count);
  bool guarded =
      in_loop && header_end > cfg->block_start[loop->header_block] &&
      f->data[header_end - 1].op == NYIR_BR_IF &&
      block_for_label(cfg, f->data[header_end - 1].imm, &body_entry);
  size_t at_block = cfg->inst_block[at];
  bool on_body_path =
      guarded && body_entry < cfg->block_count && members[body_entry] &&
      at_block < cfg->block_count && members[at_block] &&
      nyr_cfg_dominates(cfg, body_entry, at_block);
  free(members);
  return on_body_path;
}

/*
 * Symbolic linear form in the loop's exit value: `scale * exit + constant`.
 * This is the analysis-side symbolic range layer; it is never serialized.
 */
typedef struct {
  bool uses_exit;
  int64_t scale;
  int64_t constant;
} scev_exit_form_t;

static bool scev_exit_form_combine(scev_exit_form_t *out,
                                   const scev_exit_form_t *a,
                                   const scev_exit_form_t *b, int sign) {
  __int128 scale = (__int128)a->scale + sign * (__int128)b->scale;
  __int128 constant = (__int128)a->constant + sign * (__int128)b->constant;
  if (scale < INT64_MIN || scale > INT64_MAX || constant < INT64_MIN ||
      constant > INT64_MAX)
    return false;
  *out = (scev_exit_form_t){.uses_exit = a->uses_exit || b->uses_exit,
                            .scale = (int64_t)scale,
                            .constant = (int64_t)constant};
  return true;
}

static bool scev_exit_form_build(const nyr_func_t *f, const int *defs, int iv,
                                 int64_t exit_const, int value,
                                 unsigned depth, scev_exit_form_t *out) {
  if (!f || !defs || !out || depth > 32)
    return false;
  value = root_copy(f, defs, value);
  if (value == iv) {
    *out = (scev_exit_form_t){
        .uses_exit = true, .scale = 1, .constant = exit_const};
    return true;
  }
  int di = def_index(f, defs, value);
  if (di < 0 || (size_t)di >= f->len)
    return false;
  const nyr_inst_t *in = &f->data[di];
  if (in->op == NYIR_CONST_I64) {
    *out = (scev_exit_form_t){.scale = 0, .constant = in->imm};
    return true;
  }
  scev_exit_form_t a = {0}, b = {0};
  if (in->op == NYIR_ADD_I64 &&
      scev_exit_form_build(f, defs, iv, exit_const, in->a, depth + 1, &a) &&
      scev_exit_form_build(f, defs, iv, exit_const, in->b, depth + 1, &b))
    return scev_exit_form_combine(out, &a, &b, 1);
  if (in->op == NYIR_SUB_I64 &&
      scev_exit_form_build(f, defs, iv, exit_const, in->a, depth + 1, &a) &&
      scev_exit_form_build(f, defs, iv, exit_const, in->b, depth + 1, &b))
    return scev_exit_form_combine(out, &a, &b, -1);
  if (in->op == NYIR_MUL_I64) {
    int variable = in->a;
    int64_t scale = 0;
    if (!const_value(f, defs, in->b, &scale)) {
      variable = in->b;
      if (!const_value(f, defs, in->a, &scale))
        return false;
    }
    if (!scev_exit_form_build(f, defs, iv, exit_const, variable, depth + 1, &a))
      return false;
    __int128 new_scale = (__int128)a.scale * scale;
    __int128 new_const = (__int128)a.constant * scale;
    if (new_scale < INT64_MIN || new_scale > INT64_MAX ||
        new_const < INT64_MIN || new_const > INT64_MAX)
      return false;
    *out = (scev_exit_form_t){.uses_exit = a.uses_exit,
                              .scale = (int64_t)new_scale,
                              .constant = (int64_t)new_const};
    return true;
  }
  if (in->op == NYIR_SHL_I64) {
    int64_t shift = 0;
    if (!const_value(f, defs, in->b, &shift) || shift < 0 || shift >= 63)
      return false;
    if (!scev_exit_form_build(f, defs, iv, exit_const, in->a, depth + 1, &a))
      return false;
    int64_t scale = INT64_C(1) << (unsigned)shift;
    __int128 new_scale = (__int128)a.scale * scale;
    __int128 new_const = (__int128)a.constant * scale;
    if (new_scale < INT64_MIN || new_scale > INT64_MAX ||
        new_const < INT64_MIN || new_const > INT64_MAX)
      return false;
    *out = (scev_exit_form_t){.uses_exit = a.uses_exit,
                              .scale = (int64_t)new_scale,
                              .constant = (int64_t)new_const};
    return true;
  }
  return false;
}

/*
 * Post-loop proof. The induction variable seen after a single-exit counted
 * loop is the exit value (`limit` for `<`, `limit + 1` for `<=`). When both
 * the checked offset and the checked length are linear in that value with the
 * same positive scale, their difference is the constant difference, so
 * `offset < length` can be proved with no constant range on the limit. The
 * offset's lower bound uses `exit_lb`, which the entry proof tightened to at
 * least `init + 1` for `<`.
 */
static bool scev_exit_proves_check(const nyr_func_t *f,
                                   const nyr_scev_info_t *info,
                                   const int *defs, const nyr_cfg_t *cfg,
                                   size_t at, const nyr_inst_t *check) {
  if (!f || !info || !defs || !cfg || !check || check->b < 0 ||
      check->c < 0 || at >= f->len ||
      getenv("NYTRIX_NO_SCEV_EXIT_VALUE"))
    return false;
  size_t at_block = cfg->inst_block[at];
  if (at_block >= cfg->block_count)
    return false;
  bool *in_loop = calloc(cfg->block_count, sizeof(*in_loop));
  if (!in_loop)
    return false;
  bool proved = false;
  for (size_t li = 0; li < info->count && !proved; ++li) {
    const nyr_scev_loop_t *loop = &info->loops[li];
    if (!loop->exit_value_known || !loop->exit_lb_known)
      continue;
    if (!nyr_cfg_natural_loop_blocks(cfg, loop->latch_block,
                                     loop->header_block, in_loop,
                                     cfg->block_count))
      continue;
    if (in_loop[at_block])
      continue;
    bool single_exit = true;
    size_t sole_exit = SIZE_MAX;
    for (size_t b = 0; b < cfg->block_count && single_exit; ++b) {
      if (!in_loop[b])
        continue;
      for (size_t e = cfg->succ_offsets[b]; e < cfg->succ_offsets[b + 1];
           ++e) {
        size_t succ = cfg->succ_blocks[e];
        if (in_loop[succ])
          continue;
        /*
         * The exit value is `limit` only if the tested header edge is the
         * only way out. A break edge to the same exit block carries an
         * intermediate IV, and treating it as the limit would drop a live
         * check on that path.
         */
        if (b != loop->header_block ||
            (sole_exit != SIZE_MAX && sole_exit != succ)) {
          single_exit = false;
          break;
        }
        sole_exit = succ;
      }
    }
    if (!single_exit || sole_exit == SIZE_MAX ||
        !nyr_cfg_dominates(cfg, sole_exit, at_block))
      continue;
    scev_exit_form_t offset = {0}, length = {0};
    if (!scev_exit_form_build(f, defs, loop->iv, loop->exit_value_const,
                              check->b, 0, &offset) ||
        !scev_exit_form_build(f, defs, loop->iv, loop->exit_value_const,
                              check->c, 0, &length))
      continue;
    if (!offset.uses_exit || !length.uses_exit || offset.scale <= 0 ||
        offset.scale != length.scale ||
        offset.constant >= length.constant)
      continue;
    __int128 low = (__int128)offset.scale * loop->exit_lb + offset.constant;
    if (low < 0)
      continue;
    proved = true;
  }
  free(in_loop);
  return proved;
}

bool nyr_scev_tbuf_bounds_check_safe_cfg(const nyr_func_t *f,
                                         const nyr_scev_info_t *info,
                                         const int *defs, const nyr_cfg_t *cfg,
                                         size_t at, const nyr_inst_t *check) {
  if (!f || !info || !defs || !cfg || !check ||
      check->op != NYIR_BOUNDS_CHECK || check->a < 0 || check->b < 0)
    return false;
  bool *active = calloc((size_t)f->next_value, sizeof(*active));
  if (!active)
    return false;
  bool payload = scev_tbuf_payload(f, defs, check->a, active, 0);
  free(active);
  if (!payload)
    return false;
  /*
   * Constant proof: the offset's proven interval stays inside the length.
   */
  nyr_scev_affine_t offset = {0};
  if (nyr_scev_affine_at_cfg(f, info, defs, cfg, check->b, at, &offset) &&
      offset.range.has_min && offset.range.has_max && offset.range.min >= 0) {
    int64_t length_min = check->imm;
    bool have_length = true;
    if (check->c >= 0) {
      nyr_scev_affine_t length = {0};
      if (nyr_scev_affine_at_cfg(f, info, defs, cfg, check->c, at, &length) &&
          length.range.has_min)
        length_min = length.range.min;
      else
        have_length = false;
    }
    if (have_length && length_min > 0 && offset.range.max < length_min)
      return true;
  }
  /*
   * Symbolic proof from the loop's own exit test, inside the body.
   */
  if (check->c >= 0 && scev_guard_proves_offset(f, info, defs, cfg, at, check))
    return true;
  /*
   * Symbolic proof from the value the exit block observes.
   */
  return scev_exit_proves_check(f, info, defs, cfg, at, check);
}

bool nyr_scev_tbuf_bounds_check_safe(const nyr_func_t *f,
                                     const nyr_scev_info_t *info,
                                     const int *defs, size_t at,
                                     const nyr_inst_t *check) {
  if (!f || !info || !defs || at >= f->len)
    return false;
  nyr_cfg_t cfg = {0};
  if (!nyr_cfg_build(f, &cfg))
    return false;
  bool safe =
      nyr_scev_tbuf_bounds_check_safe_cfg(f, info, defs, &cfg, at, check);
  nyr_cfg_free(&cfg);
  return safe;
}

/*
 * Fold `iv % d` to zero inside a counted loop when the loop's induction
 * values are all multiples of `d`. The multiple fact (compute_multiple) only
 * exists for a constant start and step with a wrap-free recurrence, so the
 * remainder is exactly zero. `% 0` and the i64 `INT64_MIN % -1` trap are
 * excluded by requiring |d| >= 2. NYTRIX_NO_SCEV_MULTIPLE_FOLD=1 disables it.
 */
static void scev_fold_iv_multiples(nyr_func_t *f, const nyr_scev_info_t *info,
                                   const nyr_cfg_t *cfg, const int *defs) {
  for (size_t li = 0; li < info->count; ++li) {
    const nyr_scev_loop_t *l = &info->loops[li];
    if (!l->iv_multiple_known || l->iv_multiple < 2 || l->iv < 0)
      continue;
    bool *in_loop = calloc(cfg->block_count, sizeof(*in_loop));
    if (!in_loop)
      return;
    if (!nyr_cfg_natural_loop_blocks(cfg, l->latch_block, l->header_block,
                                     in_loop, cfg->block_count)) {
      free(in_loop);
      continue;
    }
    for (size_t b = 0; b < cfg->block_count; ++b) {
      if (!in_loop[b])
        continue;
      for (size_t i = cfg->block_start[b]; i < cfg->block_end[b]; ++i) {
        nyr_inst_t *in = &f->data[i];
        if (in->op != NYIR_MOD_I64 || in->a < 0 || in->b < 0)
          continue;
        if (root_copy(f, defs, in->a) != l->iv)
          continue;
        int64_t divisor = 0;
        if (!const_value(f, defs, in->b, &divisor) || divisor == 0)
          continue;
        uint64_t magnitude = scev_magnitude_i64(divisor);
        if (magnitude < 2 ||
            (uint64_t)l->iv_multiple % magnitude != 0)
          continue;
        /*
         * MOD owns no side buffers and cannot trap for |d| >= 2, so the
         * instruction can become a zero constant in place.
         */
        nyr_inst_t repl = {.op = NYIR_CONST_I64,
                           .dst = in->dst,
                           .a = -1,
                           .b = -1,
                           .c = -1,
                           .d = -1,
                           .e = -1,
                           .f = -1,
                           .imm = 0,
                           .debug = in->debug,
                           .scalar_class = in->scalar_class,
                           .range = {.has_min = true,
                                     .has_max = true,
                                     .min = 0,
                                     .max = 0}};
        *in = repl;
      }
    }
    free(in_loop);
  }
}

/*
 * Materialize proven IV intervals as verifier-visible range facts.
 */
bool nyr_scev_lite(nyr_func_t *f) {
  /*
   * This pass records facts about SSA induction values. Calls may mutate
   * reachable memory, but they cannot mutate an already-defined SSA value.
   * nyr_scev_analyze() only accepts recurrences and loop-control operands
   * whose definitions are explicit in the CFG, so unrelated calls do not
   * invalidate a proven IV interval. Memory-derived limits that are reloaded
   * in the loop are not accepted as invariant limits in the first place.
   */
  nyr_scev_info_t info = {0};
  if (!nyr_scev_analyze(f, &info))
    return false;
  for (size_t i = 0; i < info.count; ++i) {
    nyr_scev_loop_t *l = &info.loops[i];
    if (!l->trip_count_known || l->trip_count == 0) {
      /*
       * No exact count, but range facts may still bound the induction
       * variable over the whole loop, final test included. Export that
       * interval; it is strictly weaker than an exhaustion singleton.
       */
      if (!l->iv_bounds_known)
        continue;
      for (size_t j = l->header_index; j < f->len; ++j) {
        if (f->data[j].dst != l->iv)
          continue;
        f->data[j].range.has_min = true;
        f->data[j].range.has_max = true;
        f->data[j].range.min = l->iv_min;
        f->data[j].range.max = l->iv_max;
        break;
      }
      continue;
    }
    /*
     * The IV is tested once more after the final body iteration.  A range
     * ending at `last` incorrectly folds the exit comparison to true for
     * `<`/`>` loops, deleting the exit edge and producing an infinite loop.
     * Include that final tested value (`init + trip*step`) in the header
     * range.  Use wide arithmetic and discard the fact on overflow.
     */
    __int128 exit_value = (__int128)l->init + (__int128)l->trip_count * l->step;
    if (exit_value < INT64_MIN || exit_value > INT64_MAX)
      continue;
    int64_t exit_i = (int64_t)exit_value;
    for (size_t j = l->header_index; j < f->len; ++j) {
      if (f->data[j].dst != l->iv)
        continue;
      f->data[j].range.has_min = true;
      f->data[j].range.has_max = true;
      if (l->step > 0) {
        f->data[j].range.min = l->init;
        f->data[j].range.max = exit_i;
      } else {
        f->data[j].range.min = exit_i;
        f->data[j].range.max = l->init;
      }
      break;
    }
  }
  /*
   * Known-multiple consumer: replace `iv % d` with zero where divisibility is
   * proven. This runs here because SCEV is the only analysis that knows the
   * recurrence is wrap-free.
   */
  nyr_cfg_t cfg = {0};
  int *defs = NULL;
  bool have_cfg = false;
  if (!getenv("NYTRIX_NO_SCEV_MULTIPLE_FOLD")) {
    have_cfg = nyr_cfg_build(f, &cfg);
    if (have_cfg) {
      defs = nyr_build_defs(f);
      if (!defs) {
        nyr_cfg_free(&cfg);
        have_cfg = false;
      }
    }
  }
  if (have_cfg)
    scev_fold_iv_multiples(f, &info, &cfg, defs);
  free(defs);
  if (have_cfg)
    nyr_cfg_free(&cfg);
  nyr_scev_free(&info);
  return true;
}
