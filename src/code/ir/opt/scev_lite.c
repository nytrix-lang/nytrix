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
#include "code/ir/opt/loop.h"
#include "code/ir/opt/util.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static int root_copy(const nyir_func_t *f, const int *defs, int value) {
  for (int depth = 0; value >= 0 && value < f->next_value &&
                      defs[value] >= 0 &&
                      f->data[defs[value]].op == NYIR_COPY && depth < 32;
       ++depth)
    value = f->data[defs[value]].a;
  return value;
}

static int def_index(const nyir_func_t *f, const int *defs, int value) {
  value = root_copy(f, defs, value);
  return value >= 0 && value < f->next_value ? defs[value] : -1;
}

static bool const_value(const nyir_func_t *f, const int *defs, int value,
                        int64_t *out) {
  int di = def_index(f, defs, value);
  if (di < 0 || f->data[di].op != NYIR_CONST_I64)
    return false;
  *out = f->data[di].imm;
  return true;
}

static bool compute_trip(nyir_scev_loop_t *l) {
  if (!l->limit_is_const || l->step == 0)
    return false;
  __int128 start = l->init, limit = l->limit, step = l->step, n = 0;
  if (step > 0) {
    if (l->predicate == NYIR_CMP_LT) {
      if (start >= limit) n = 0;
      else n = (limit - start + step - 1) / step;
    } else if (l->predicate == NYIR_CMP_LE) {
      if (start > limit) n = 0;
      else n = (limit - start) / step + 1;
    } else return false;
  } else {
    __int128 mag = -step;
    if (l->predicate == NYIR_CMP_GT) {
      if (start <= limit) n = 0;
      else n = (start - limit + mag - 1) / mag;
    } else if (l->predicate == NYIR_CMP_GE) {
      if (start < limit) n = 0;
      else n = (start - limit) / mag + 1;
    } else return false;
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

static size_t block_last_non_nop(const nyir_func_t *f, const nyir_cfg_t *cfg,
                                 size_t block) {
  size_t end = cfg->block_end[block];
  while (end > cfg->block_start[block] && f->data[end - 1u].op == NYIR_NOP)
    --end;
  return end;
}

static bool block_for_label(const nyir_cfg_t *cfg, int64_t label,
                            size_t *out_block) {
  for (size_t block = 0; cfg && block < cfg->block_count; ++block) {
    if (cfg->block_label[block] != label)
      continue;
    if (out_block)
      *out_block = block;
    return true;
  }
  return false;
}

static bool append_loop(nyir_scev_info_t *out, const nyir_scev_loop_t *loop) {
  if (out->count == SIZE_MAX / sizeof(*out->loops))
    return false;
  nyir_scev_loop_t *p = realloc(out->loops,
      (out->count + 1) * sizeof(*out->loops));
  if (!p)
    return false;
  out->loops = p;
  out->loops[out->count++] = *loop;
  return true;
}

bool nyir_scev_analyze(const nyir_func_t *f, nyir_scev_info_t *out) {
  if (!out)
    return false;
  memset(out, 0, sizeof(*out));
  if (!f || f->next_value <= 0)
    return true;
  nyir_cfg_t cfg = {0};
  int *defs = nyir_build_defs(f);
  if (!defs || !nyir_cfg_build(f, &cfg)) {
    free(defs);
    return false;
  }
  for (size_t latch = 0; latch < cfg.block_count; ++latch) {
    for (size_t e = cfg.succ_offsets[latch]; e < cfg.succ_offsets[latch + 1]; ++e) {
      size_t header = cfg.succ_blocks[e];
      if (!nyir_cfg_is_backedge(&cfg, latch, header))
        continue;
      size_t preheader = SIZE_MAX, outside = 0;
      for (size_t p = cfg.pred_offsets[header]; p < cfg.pred_offsets[header + 1]; ++p) {
        size_t pred = cfg.pred_blocks[p];
        if (!nyir_cfg_dominates(&cfg, header, pred)) {
          preheader = pred;
          outside++;
        }
      }
      if (outside != 1)
        continue;
      bool *in_loop = calloc(cfg.block_count, sizeof(*in_loop));
      if (!in_loop) {
        nyir_scev_free(out);
        nyir_cfg_free(&cfg);
        free(defs);
        return false;
      }
      size_t header_end = block_last_non_nop(f, &cfg, header);
      const nyir_inst_t *control =
          header_end > cfg.block_start[header] ? &f->data[header_end - 1u] : NULL;
      size_t taken_block = SIZE_MAX;
      if (!nyir_cfg_natural_loop_blocks(&cfg, latch, header, in_loop,
                                      cfg.block_count) || !control ||
          control->op != NYIR_BR_IF ||
          !block_for_label(&cfg, control->imm, &taken_block) ||
          !in_loop[taken_block]) {
        free(in_loop);
        continue;
      }
uint64_t trip_count = 0;
      bool trip_known = false;

      for (size_t pass = 0; pass < 2; ++pass) {
        for (size_t i = cfg.block_start[header]; i < cfg.block_end[header]; ++i) {
          const nyir_inst_t *phi = &f->data[i];
          if (phi->op != NYIR_PHI || phi->phi_incoming_len != 2)
            continue;
          int init = -1, next = -1;
          for (size_t k = 0; k < 2; ++k) {
            int64_t label = phi->phi_incoming[k].predecessor_label;
            int v = root_copy(f, defs, phi->phi_incoming[k].value);
            if (label == cfg.block_label[preheader]) init = v;
            if (label == cfg.block_label[latch]) next = v;
          }
          if (init < 0 || next < 0)
            continue;
          int ni = def_index(f, defs, next);
          if (ni < 0)
            continue;
          const nyir_inst_t *inc = &f->data[ni];
          int stepv = -1;
          int64_t sign = 1;
          if (inc->op == NYIR_ADD_I64 &&
              root_copy(f, defs, inc->a) == phi->dst)
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
          if (!const_value(f, defs, stepv, &step) ||
              !const_value(f, defs, init, &start) || step == 0)
            continue;
          if (sign < 0 && step == INT64_MIN)
            continue;
          step *= sign;

          if (pass == 0) {
            for (size_t j = cfg.block_start[header]; j < cfg.block_end[header]; ++j) {
              const nyir_inst_t *cmp = &f->data[j];
              if (cmp->op != NYIR_CMP_I64 || cmp->dst < 0 ||
                  root_copy(f, defs, cmp->a) != phi->dst ||
                  root_copy(f, defs, control->a) != cmp->dst)
                continue;
              nyir_scev_loop_t loop = {.header_block = header,
                .latch_block = latch, .preheader_block = preheader,
                .header_index = cfg.block_start[header],
                .latch_index = cfg.block_end[latch] - 1u,
                .header_label = cfg.block_label[header], .iv = phi->dst,
                .init_value = init, .next_value = next, .limit_value = cmp->b,
                .init = start, .step = step, .predicate = cmp->cmp};
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
            nyir_scev_loop_t loop = {.header_block = header,
              .latch_block = latch, .preheader_block = preheader,
              .header_index = cfg.block_start[header],
              .latch_index = cfg.block_end[latch] - 1u,
              .header_label = cfg.block_label[header], .iv = phi->dst,
              .init_value = init, .next_value = next, .limit_value = -1,
              .init = start, .step = step, .predicate = 0};
            for (size_t j = cfg.block_start[header]; j < cfg.block_end[header]; ++j) {
              const nyir_inst_t *cmp = &f->data[j];
              if (cmp->op != NYIR_CMP_I64 || cmp->dst < 0 ||
                  root_copy(f, defs, cmp->a) != phi->dst ||
                  root_copy(f, defs, control->a) != cmp->dst)
                continue;
              loop.limit_value = cmp->b;
              loop.predicate = cmp->cmp;
              loop.limit_is_const = const_value(f, defs, cmp->b, &loop.limit);
              is_primary = true;
              break;
            }
            if (is_primary) {
               compute_trip(&loop);
            } else if (trip_known) {
               loop.trip_count_known = true;
               loop.trip_count = trip_count;
            }
            if (!append_loop(out, &loop)) {
              free(in_loop);
              nyir_scev_free(out);
              nyir_cfg_free(&cfg);
              free(defs);
              return false;
            }
          }
        }
      }
      free(in_loop);

    }
  }
  nyir_cfg_free(&cfg);
  free(defs);
  return true;
}

void nyir_scev_free(nyir_scev_info_t *info) {
  if (!info) return;
  free(info->loops);
  memset(info, 0, sizeof(*info));
}

static bool scev_loop_iv_bounds_at(const nyir_func_t *f,
                                   const nyir_scev_info_t *info,
                                   const nyir_cfg_t *cfg, int value, size_t at,
                                   int64_t *min_out, int64_t *max_out) {
  if (!f || !info || !cfg || !min_out || !max_out || at >= f->len)
    return false;
  size_t at_block = cfg->inst_block[at];
  for (size_t i = 0; i < info->count; ++i) {
    const nyir_scev_loop_t *loop = &info->loops[i];
    if (loop->iv != value || !loop->trip_count_known ||
        loop->trip_count == 0)
      continue;
    __int128 last = (__int128)loop->init +
                    (__int128)(loop->trip_count - 1) * loop->step;
    __int128 exit = last + loop->step;
    if (last < INT64_MIN || last > INT64_MAX || exit < INT64_MIN ||
        exit > INT64_MAX)
      continue;
    bool *in_loop = calloc(cfg->block_count, sizeof(*in_loop));
    if (!in_loop)
      return false;
    bool in_body = nyir_cfg_natural_loop_blocks(
        cfg, loop->latch_block, loop->header_block, in_loop, cfg->block_count) &&
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
      for (size_t e = cfg->succ_offsets[b];
           e < cfg->succ_offsets[b + 1]; ++e) {
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
        nyir_cfg_dominates(cfg, sole_exit, at_block)) {
      *min_out = (int64_t)exit;
      *max_out = (int64_t)exit;
      return true;
    }
  }
  return false;
}

static void scev_affine_init(nyir_scev_affine_t *out) {
  *out = (nyir_scev_affine_t){.iv = {-1, -1}};
}

static bool scev_affine_add_term(nyir_scev_affine_t *out, int iv,
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

static bool scev_affine_combine(nyir_scev_affine_t *out,
                                const nyir_scev_affine_t *a,
                                const nyir_scev_affine_t *b, int sign) {
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

static bool scev_affine_scale(nyir_scev_affine_t *out,
                              const nyir_scev_affine_t *in, int64_t scale) {
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

static bool scev_affine_build(const nyir_func_t *f,
                              const nyir_scev_info_t *info, const int *defs,
                              const nyir_cfg_t *cfg, int value, size_t at,
                              unsigned depth, nyir_scev_affine_t *out) {
  if (!f || !info || !defs || !cfg || !out || depth > 32)
    return false;
  value = root_copy(f, defs, value);
  if (value < 0 || value >= f->next_value)
    return false;
  int64_t lo = 0, hi = 0;
  if (scev_loop_iv_bounds_at(f, info, cfg, value, at, &lo, &hi)) {
    scev_affine_init(out);
    return scev_affine_add_term(out, value, 1);
  }
  int di = defs[value];
  if (di < 0 || (size_t)di >= f->len)
    return false;
  const nyir_inst_t *in = &f->data[di];
  if (in->op == NYIR_CONST_I64) {
    scev_affine_init(out);
    out->constant = in->imm;
    return true;
  }
  nyir_scev_affine_t a = {0}, b = {0};
  if (in->op == NYIR_ADD_I64 &&
      scev_affine_build(f, info, defs, cfg, in->a, at, depth + 1, &a) &&
      scev_affine_build(f, info, defs, cfg, in->b, at, depth + 1, &b))
    return scev_affine_combine(out, &a, &b, 1);
  if (in->op == NYIR_SUB_I64 &&
      scev_affine_build(f, info, defs, cfg, in->a, at, depth + 1, &a) &&
      scev_affine_build(f, info, defs, cfg, in->b, at, depth + 1, &b))
    return scev_affine_combine(out, &a, &b, -1);
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

static bool scev_affine_bound(const nyir_func_t *f,
                              const nyir_scev_info_t *info,
                              const nyir_cfg_t *cfg, size_t at,
                              nyir_scev_affine_t *out) {
  if (!f || !info || !cfg || !out)
    return false;
  __int128 lo = out->constant, hi = out->constant;
  for (size_t i = 0; i < 2; ++i) {
    if (out->iv[i] < 0 || out->coefficient[i] == 0)
      continue;
    int64_t iv_min = 0, iv_max = 0;
    if (!scev_loop_iv_bounds_at(f, info, cfg, out->iv[i], at, &iv_min,
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
  out->range = (nyir_range_t){.has_min = true, .has_max = true,
                               .min = (int64_t)lo, .max = (int64_t)hi};
  return true;
}

bool nyir_scev_affine_at(const nyir_func_t *f, const nyir_scev_info_t *info,
                         const int *defs, int value, size_t at,
                         nyir_scev_affine_t *out) {
  if (!f || !info || !defs || !out || at >= f->len)
    return false;
  nyir_cfg_t cfg = {0};
  if (!nyir_cfg_build(f, &cfg))
    return false;
  bool ok = scev_affine_build(f, info, defs, &cfg, value, at, 0, out) &&
            scev_affine_bound(f, info, &cfg, at, out);
  nyir_cfg_free(&cfg);
  return ok;
}

static bool scev_tbuf_payload(const nyir_func_t *f, const int *defs,
                              int value, bool *active, unsigned depth) {
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
  const nyir_inst_t *in = &f->data[di];
  if (in->op == NYIR_ALLOCA || in->op == NYIR_ADDR_LOCAL ||
      in->op == NYIR_ADDR_SYMBOL)
    return true;
  if (in->op == NYIR_CALL && in->symbol) {
    if (strcmp(in->symbol, "rt_tbuf_new_raw") == 0 ||
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

bool nyir_scev_tbuf_bounds_check_safe(const nyir_func_t *f,
                                      const nyir_scev_info_t *info,
                                      const int *defs, size_t at,
                                      const nyir_inst_t *check) {
  if (!f || !info || !defs || !check || check->op != NYIR_BOUNDS_CHECK ||
      check->a < 0 || check->b < 0)
    return false;
  bool *active = calloc((size_t)f->next_value, sizeof(*active));
  if (!active)
    return false;
  bool payload = scev_tbuf_payload(f, defs, check->a, active, 0);
  free(active);
  if (!payload)
    return false;
  nyir_scev_affine_t offset = {0};
  if (!nyir_scev_affine_at(f, info, defs, check->b, at, &offset) ||
      !offset.range.has_min || !offset.range.has_max || offset.range.min < 0)
    return false;
  int64_t length_min = check->imm;
  if (check->c >= 0) {
    nyir_scev_affine_t length = {0};
    if (!nyir_scev_affine_at(f, info, defs, check->c, at, &length) ||
        !length.range.has_min)
      return false;
    length_min = length.range.min;
  }
  return length_min > 0 && offset.range.max < length_min;
}

/*
 * Materialize proven IV intervals as verifier-visible range facts.
 */
bool nyir_scev_lite(nyir_func_t *f) {
  /*
   * This pass records facts about SSA induction values. Calls may mutate
   * reachable memory, but they cannot mutate an already-defined SSA value.
   * nyir_scev_analyze() only accepts recurrences and loop-control operands
   * whose definitions are explicit in the CFG, so unrelated calls do not
   * invalidate a proven IV interval. Memory-derived limits that are reloaded
   * in the loop are not accepted as invariant limits in the first place.
   */
  nyir_scev_info_t info = {0};
  if (!nyir_scev_analyze(f, &info))
    return false;
  for (size_t i = 0; i < info.count; ++i) {
    nyir_scev_loop_t *l = &info.loops[i];
    if (!l->trip_count_known || l->trip_count == 0)
      continue;
    /*
     * The IV is tested once more after the final body iteration.  A range
     * ending at `last` incorrectly folds the exit comparison to true for
     * `<`/`>` loops, deleting the exit edge and producing an infinite loop.
     * Include that final tested value (`init + trip*step`) in the header
     * range.  Use wide arithmetic and discard the fact on overflow.
     */
    __int128 exit_value = (__int128)l->init +
                          (__int128)l->trip_count * l->step;
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
  nyir_scev_free(&info);
  return true;
}
