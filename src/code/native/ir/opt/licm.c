/*
 * Loop-invariant code motion over CFG-proven natural loops.
 */
#include "code/native/ir/opt/util.h"
#include "code/native/ir/opt/loop_analysis.h"
#include "code/native/ir/internal.h"
#include "base/compat.h"
#include "base/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  size_t index;
  nyir_inst_t inst;
} nyir_licm_move_t;

static bool licm_find_preheader(const nyir_cfg_t *cfg, const bool *in_loop,
                                size_t header, size_t *out_preheader) {
  if (!cfg || !in_loop || !out_preheader || header >= cfg->block_count)
    return false;
  size_t preheader = SIZE_MAX;
  size_t outside = 0;
  for (size_t p = cfg->pred_offsets[header]; p < cfg->pred_offsets[header + 1];
       ++p) {
    size_t pred = cfg->pred_blocks[p];
    if (in_loop[pred])
      continue;
    preheader = pred;
    outside++;
  }
  if (outside != 1 || preheader >= cfg->block_count)
    return false;
  /*
   * A dedicated single-successor preheader is unnecessary for instructions
   * that licm_candidate has already proven pure and non-trapping.  Hoisting
   * into the unique outside predecessor is safe even when it also has a
   * loop-bypass edge.
   */
  *out_preheader = preheader;
  return true;
}

static size_t licm_insert_position(const nyir_func_t *f,
                                   const nyir_cfg_t *cfg,
                                   size_t preheader) {
  size_t start = cfg->block_start[preheader];
  size_t end = cfg->block_end[preheader];
  /*
   * Conditional blocks may contain BR_IF followed by a fallback BR.  Insert
   * before the first terminator, never between the two terminators.
   */
  for (size_t i = start; i < end; ++i) {
    nyir_op_t op = f->data[i].op;
    if (op == NYIR_BR || op == NYIR_BR_IF || op == NYIR_RET)
      return i;
  }
  while (end > start && f->data[end - 1u].op == NYIR_NOP)
    --end;
  return end;
}

static bool licm_operand_invariant(int value, int value_count,
                                   const bool *loop_defined,
                                   const bool *scheduled) {
  if (value < 0 || value >= value_count)
    return true;
  return !loop_defined[value] || scheduled[value];
}

static bool licm_candidate(const nyir_inst_t *in, bool loop_writes_memory,
                           bool stable_managed_read,
                           bool trap_equivalent_read,
                           const bool *stored_local, size_t local_count,
                           int value_count, const bool *loop_defined,
                           const bool *scheduled) {
  if (!in)
    return false;
  if (in->op == NYIR_BOUNDS_CHECK) {
    if (!trap_equivalent_read)
      return false;
    const int inputs[] = {in->a, in->b, in->c};
    for (size_t k = 0; k < sizeof(inputs) / sizeof(inputs[0]); ++k)
      if (!licm_operand_invariant(inputs[k], value_count, loop_defined,
                                  scheduled))
        return false;
    return true;
  }
  if (in->dst < 0)
    return false;
  unsigned effects = nyir_effective_effects(in);
  const unsigned forbidden =
      NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_WRITE_LOCAL |
      NYIR_EFFECT_CONTROL | NYIR_EFFECT_VOLATILE | NYIR_EFFECT_ALLOCATION |
      NYIR_EFFECT_UNKNOWN_SIDE_EFFECT | NYIR_EFFECT_IO | NYIR_EFFECT_THREAD |
      NYIR_EFFECT_FFI | NYIR_EFFECT_FENV;
  if (effects & forbidden)
    return false;
  if ((effects & NYIR_EFFECT_MAY_TRAP) && !trap_equivalent_read)
    return false;
  if (trap_equivalent_read && in->op == NYIR_CALL &&
      !(effects & NYIR_EFFECT_READ_MEMORY))
    return false;
  if ((effects & NYIR_EFFECT_READ_MEMORY) && loop_writes_memory &&
      !stable_managed_read)
    return false;
  if (in->op == NYIR_CALL) {
    unsigned allowed = NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY;
    if (trap_equivalent_read)
      allowed |= NYIR_EFFECT_MAY_TRAP;
    if ((effects & ~allowed) != 0)
      return false;
  }
  switch (in->op) {
  case NYIR_LABEL:
  case NYIR_PHI:
  case NYIR_BR:
  case NYIR_BR_IF:
  case NYIR_RET:
  case NYIR_STORE_I64:
  case NYIR_ADDR_LOCAL:
  case NYIR_STORE_LOCAL:
    return false;
  default:
    break;
  }
  if (in->op == NYIR_LOAD_LOCAL && in->imm >= 0 &&
      (size_t)in->imm < local_count && stored_local &&
      stored_local[in->imm])
    return false;
  const int inputs[] = {in->a, in->b, in->c, in->d, in->e, in->f};
  for (size_t k = 0; k < sizeof(inputs) / sizeof(inputs[0]); ++k)
    if (!licm_operand_invariant(inputs[k], value_count, loop_defined,
                                scheduled))
      return false;
  for (size_t k = 0; k < in->extra_args_len; ++k)
    if (!licm_operand_invariant(in->extra_args[k], value_count, loop_defined,
                                scheduled))
      return false;
  return true;
}

/*
 * A read-only helper that may fault on an invalid raw pointer can be moved
 * into the preheader only when doing so is trap-equivalent.  A statically
 * positive trip count guarantees the loop executes.  Dominance of the latch
 * and every non-header exit guarantees the original call executed on every
 * path that can leave/continue the first iteration, so hoisting neither adds
 * a new trapping path nor suppresses one.
 */
static bool licm_trap_equivalent_read(const nyir_cfg_t *cfg,
                                      const bool *in_loop, size_t header,
                                      size_t latch, size_t call_block,
                                      bool positive_trip) {
  if (!cfg || !in_loop || !positive_trip || header >= cfg->block_count ||
      latch >= cfg->block_count || call_block >= cfg->block_count ||
      !nyir_cfg_dominates(cfg, call_block, latch))
    return false;
  for (size_t block = 0; block < cfg->block_count; ++block) {
    if (!in_loop[block] || block == header)
      continue;
    for (size_t e = cfg->succ_offsets[block]; e < cfg->succ_offsets[block + 1];
         ++e) {
      size_t succ = cfg->succ_blocks[e];
      if (!in_loop[succ] && !nyir_cfg_dominates(cfg, call_block, block))
        return false;
    }
  }
  return true;
}


static bool licm_addr_from_tbuf_data(const nyir_func_t *f,
                                     const nyir_value_fact_t *facts,
                                     const int *defs, int addr, int base,
                                     unsigned depth) {
  if (!f || !facts || !defs || addr < 0 || base < 0 || depth > 16)
    return false;
  if (addr == base)
    return true;
  if (addr >= f->next_value || defs[addr] < 0 || (size_t)defs[addr] >= f->len)
    return false;
  const nyir_inst_t *in = &f->data[defs[addr]];
  if (in->op == NYIR_COPY)
    return licm_addr_from_tbuf_data(f, facts, defs, in->a, base, depth + 1);
  if (in->op != NYIR_ADD_I64)
    return false;
  int derived = -1, offset = -1;
  if (licm_addr_from_tbuf_data(f, facts, defs, in->a, base, depth + 1)) {
    derived = in->a;
    offset = in->b;
  } else if (licm_addr_from_tbuf_data(f, facts, defs, in->b, base, depth + 1)) {
    derived = in->b;
    offset = in->a;
  }
  (void)derived;
  if (offset < 0 || offset >= f->next_value)
    return false;
  const nyir_value_fact_t *fact = &facts[offset];
  return fact->range.has_min && fact->range.min >= 0;
}

/*
 * rt_native_tbuf_len reads only the count word at data[-24].  Element stores
 * through data + nonnegative_offset cannot modify that header.  This lets
 * dense update loops hoist one invariant length read even though the loop
 * legitimately writes the buffer payload.  Any unknown write/call, negative
 * pointer arithmetic, or store through another base remains a hard barrier.
 */
static bool licm_tbuf_len_header_stable(const nyir_func_t *f,
                                        const nyir_cfg_t *cfg,
                                        const bool *in_loop,
                                        const nyir_value_fact_t *facts,
                                        const int *defs, int base) {
  if (!f || !cfg || !in_loop || !facts || !defs || base < 0)
    return false;
  for (size_t block = 0; block < cfg->block_count; ++block) {
    if (!in_loop[block])
      continue;
    for (size_t i = cfg->block_start[block]; i < cfg->block_end[block]; ++i) {
      const nyir_inst_t *in = &f->data[i];
      unsigned effects = nyir_effective_effects(in);
      if (!(effects & NYIR_EFFECT_WRITE_MEMORY))
        continue;
      if (in->op == NYIR_STORE_I64 && in->a >= 0 &&
          licm_addr_from_tbuf_data(f, facts, defs, in->a, base, 0))
        continue;
      return false;
    }
  }
  return true;
}

static bool licm_grow_moves(nyir_licm_move_t **moves, size_t *cap,
                            size_t need) {
  if (need <= *cap)
    return true;
  size_t next = *cap ? *cap * 2u : 16u;
  while (next < need) {
    if (next > SIZE_MAX / 2u)
      return false;
    next *= 2u;
  }
  if (next > SIZE_MAX / sizeof(**moves))
    return false;
  nyir_licm_move_t *grown = realloc(*moves, next * sizeof(*grown));
  if (!grown)
    return false;
  *moves = grown;
  *cap = next;
  return true;
}

/*
 * Hoist one natural loop's currently provable invariants.  Returning after a
 * single CFG mutation lets the caller rebuild dominance/topology before
 * considering another loop, so nested/adjacent loops never consume stale
 * block or instruction indices.
 */
static bool licm_hoist_one(nyir_func_t *f, bool *changed) {
  if (changed)
    *changed = false;
  nyir_cfg_t cfg = {0};
  nyir_scev_info_t scev = {0};
  if (!nyir_cfg_build(f, &cfg) || !nyir_scev_analyze(f, &scev)) {
    nyir_cfg_free(&cfg);
    nyir_scev_free(&scev);
    return false;
  }
  bool *in_loop = calloc(cfg.block_count, sizeof(*in_loop));
  if (!in_loop) {
    nyir_cfg_free(&cfg);
    nyir_scev_free(&scev);
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
      size_t preheader = SIZE_MAX;
      if (!licm_find_preheader(&cfg, in_loop, header, &preheader))
        continue;
      bool positive_trip = false;
      for (size_t si = 0; si < scev.count; ++si) {
        const nyir_scev_loop_t *sl = &scev.loops[si];
        if (sl->header_block == header && sl->latch_block == latch &&
            sl->trip_count_known && sl->trip_count > 0) {
          positive_trip = true;
          break;
        }
      }

      size_t local_count = nyir_max_local(f);
      bool *stored_local =
          local_count ? calloc(local_count, sizeof(*stored_local)) : NULL;
      bool *loop_defined =
          calloc((size_t)f->next_value, sizeof(*loop_defined));
      bool *scheduled = calloc((size_t)f->next_value, sizeof(*scheduled));
      bool *selected_inst = calloc(f->len, sizeof(*selected_inst));
      nyir_value_fact_t *facts =
          calloc((size_t)f->next_value, sizeof(*facts));
      int *defs = malloc((size_t)f->next_value * sizeof(*defs));
      if ((local_count && !stored_local) || !loop_defined || !scheduled ||
          !selected_inst || !facts || !defs ||
          !nyir_analyze_values(f, facts, (size_t)f->next_value, NULL, 0)) {
        free(stored_local);
        free(loop_defined);
        free(scheduled);
        free(selected_inst);
        free(facts);
        free(defs);
        free(in_loop);
        nyir_cfg_free(&cfg);
        nyir_scev_free(&scev);
        return false;
      }

      for (int v = 0; v < f->next_value; ++v)
        defs[v] = -1;
      for (size_t i = 0; i < f->len; ++i)
        if (f->data[i].dst >= 0 && f->data[i].dst < f->next_value)
          defs[f->data[i].dst] = (int)i;

      bool loop_writes_memory = false;
      for (size_t block = 0; block < cfg.block_count; ++block) {
        if (!in_loop[block])
          continue;
        for (size_t i = cfg.block_start[block]; i < cfg.block_end[block]; ++i) {
          const nyir_inst_t *in = &f->data[i];
          unsigned effects = nyir_effective_effects(in);
          if (effects & NYIR_EFFECT_WRITE_MEMORY)
            loop_writes_memory = true;
          if (in->op == NYIR_STORE_LOCAL && in->imm >= 0 &&
              (size_t)in->imm < local_count)
            stored_local[in->imm] = true;
          if (in->dst >= 0 && in->dst < f->next_value)
            loop_defined[in->dst] = true;
        }
      }

      nyir_licm_move_t *moves = NULL;
      size_t move_count = 0, move_cap = 0;
      bool progress;
      do {
        progress = false;
        for (size_t block = 0; block < cfg.block_count; ++block) {
          if (!in_loop[block])
            continue;
          for (size_t i = cfg.block_start[block]; i < cfg.block_end[block];
               ++i) {
            nyir_inst_t *in = &f->data[i];
            unsigned effects = nyir_effective_effects(in);
            bool is_may_trap = (effects & NYIR_EFFECT_MAY_TRAP) != 0;
            bool trap_equivalent_read =
                is_may_trap &&
                licm_trap_equivalent_read(&cfg, in_loop, header, latch, block,
                                          positive_trip);
            bool stable_managed_read =
                in->op == NYIR_CALL && in->symbol &&
                strcmp(in->symbol, "rt_native_tbuf_len") == 0 && in->a >= 0 &&
                licm_tbuf_len_header_stable(f, &cfg, in_loop, facts, defs,
                                            in->a);
            if (selected_inst[i] ||
                !licm_candidate(in, loop_writes_memory, stable_managed_read,
                                trap_equivalent_read, stored_local, local_count,
                                f->next_value, loop_defined, scheduled))
              continue;
            if (!licm_grow_moves(&moves, &move_cap, move_count + 1u)) {
              free(moves);
              free(stored_local);
              free(loop_defined);
              free(scheduled);
              free(selected_inst);
              free(facts);
              free(defs);
              free(in_loop);
              nyir_cfg_free(&cfg);
              nyir_scev_free(&scev);
              return false;
            }
            moves[move_count++] = (nyir_licm_move_t){i, *in};
            selected_inst[i] = true;
            if (in->dst >= 0)
              scheduled[in->dst] = true;
            progress = true;
          }
        }
      } while (progress);

      if (move_count == 0) {
        free(moves);
        free(stored_local);
        free(loop_defined);
        free(scheduled);
        free(selected_inst);
        free(facts);
        free(defs);
        continue;
      }

      size_t at = licm_insert_position(f, &cfg, preheader);
      if (move_count > SIZE_MAX - f->len) {
        free(moves);
        free(stored_local);
        free(loop_defined);
        free(scheduled);
        free(selected_inst);
        free(facts);
        free(defs);
        free(in_loop);
        nyir_cfg_free(&cfg);
        nyir_scev_free(&scev);
        return false;
      }
      size_t need = f->len + move_count;
      if (!nir_ensure_inst_space(f, move_count)) {
        free(moves);
        free(stored_local);
        free(loop_defined);
        free(scheduled);
        free(selected_inst);
        free(facts);
        free(defs);
        free(in_loop);
        nyir_cfg_free(&cfg);
        nyir_scev_free(&scev);
        return false;
      }

      /*
       * Transfer ownership of instruction metadata to the preheader copies.
       */
      for (size_t k = 0; k < move_count; ++k) {
        size_t i = moves[k].index;
        f->data[i] = (nyir_inst_t){.op = NYIR_NOP, .dst = -1, .a = -1,
                                   .b = -1, .c = -1, .d = -1, .e = -1,
                                   .f = -1};
      }
      memmove(&f->data[at + move_count], &f->data[at],
              (f->len - at) * sizeof(*f->data));
      for (size_t k = 0; k < move_count; ++k)
        f->data[at + k] = moves[k].inst;
      f->len = need;

      free(moves);
      free(stored_local);
      free(loop_defined);
      free(scheduled);
      free(selected_inst);
      free(facts);
      free(defs);
      free(in_loop);
      nyir_cfg_free(&cfg);
      nyir_scev_free(&scev);
      if (changed)
        *changed = true;
      return true;
    }
  }

  free(in_loop);
  nyir_cfg_free(&cfg);
  nyir_scev_free(&scev);
  return true;
}

bool nyir_licm(nyir_func_t *f) {
  if (!f || f->len < 4 || f->next_value <= 0)
    return true;
  bool has_loop_branches = false;
  for (size_t i = 0; i < f->len; ++i) {
    if (f->data[i].op == NYIR_BR || f->data[i].op == NYIR_BR_IF) {
      has_loop_branches = true;
      break;
    }
  }
  if (!has_loop_branches)
    return true;
  for (int iter = 0; iter < 16; ++iter) {
    bool changed = false;
    if (!licm_hoist_one(f, &changed))
      return false;
    if (!changed)
      return true;
  }
  return true;
}
