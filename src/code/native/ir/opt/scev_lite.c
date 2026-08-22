/*
 * Canonical counted-loop analysis for NyIR.
 */
#include "code/native/ir/opt/loop_analysis.h"
#include "code/native/ir/opt/util.h"
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
          if (!append_loop(out, &loop)) {
            free(in_loop);
            nyir_scev_free(out);
            nyir_cfg_free(&cfg);
            free(defs);
            return false;
          }
          break;
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
