/*
 * Dead-store elimination: removes memory stores to locations that
 * are never subsequently read before being overwritten or going dead.
 */
#include "base/common.h"
#include "base/compat.h"
#include "base/debug.h"
#include "code/backend/ir/internal.h"
#include "code/backend/ir/opt/util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Dead Store Elimination: remove STORE_LOCAL followed by another
 * STORE_LOCAL to the same slot without an intervening observation.
 *
 * Walk each basic block backwards.  This is deliberately linear in
 * the instruction stream: the previous implementation scanned ahead
 * from every store, which made store-heavy generated code quadratic.
 * Calls and control-flow boundaries remain conservative barriers.
 */

/*
 * A store is partial-dead when every continuation either overwrites that
 * slot or returns without reading it.  Keep this deliberately narrower than
 * general memory DSE: address-taken locals and all unknown memory effects
 * stop the proof.
 */
static bool dse_block_dead_after_store(const nyr_func_t *f,
                                       const nyr_cfg_t *cfg, size_t slot,
                                       size_t block, size_t start,
                                       unsigned char *state) {
  for (size_t pc = start; pc < cfg->block_end[block]; ++pc) {
    const nyr_inst_t *in = &f->data[pc];
    if (in->op == NYIR_RET ||
        (in->op == NYIR_STORE_LOCAL && in->imm == (int64_t)slot))
      return true;
    if ((in->op == NYIR_LOAD_LOCAL || in->op == NYIR_ADDR_LOCAL) &&
        in->imm == (int64_t)slot)
      return false;
    if (in->op == NYIR_CALL)
      return false;
    unsigned effects = nyr_effective_effects(in);
    if (effects & (NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_WRITE_MEMORY |
                   NYIR_EFFECT_VOLATILE | NYIR_EFFECT_UNKNOWN_SIDE_EFFECT))
      return false;
  }

  size_t first = cfg->succ_offsets[block];
  size_t last = cfg->succ_offsets[block + 1];
  if (first == last)
    return false;
  for (size_t edge = first; edge < last; ++edge) {
    size_t successor = cfg->succ_blocks[edge];
    if (state[successor] == 1)
      return false;
    if (state[successor] == 2)
      continue;
    if (state[successor] == 3)
      return false;
    state[successor] = 1;
    bool dead = dse_block_dead_after_store(f, cfg, slot, successor,
                                           cfg->block_start[successor], state);
    state[successor] = dead ? 2 : 3;
    if (!dead)
      return false;
  }
  return true;
}

static bool dse_store_dead_on_all_paths(const nyr_func_t *f,
                                        const nyr_cfg_t *cfg, size_t slot,
                                        size_t pc) {
  size_t block = cfg->inst_block[pc];
  unsigned char *state = calloc(cfg->block_count, sizeof(*state));
  if (!state || block >= cfg->block_count) {
    free(state);
    return false;
  }
  bool dead = dse_block_dead_after_store(f, cfg, slot, block, pc + 1, state);
  free(state);
  return dead;
}

static int find_alloc_root_depth(const nyr_func_t *f, const int *defs, int v,
                                 int depth) {
  if (v < 0 || v >= f->next_value || depth > 16)
    return -1;
  int di = defs[v];
  if (di < 0 || (size_t)di >= f->len)
    return -1;
  const nyr_inst_t *in = &f->data[di];
  if (in->op == NYIR_COPY)
    return find_alloc_root_depth(f, defs, in->a, depth + 1);
  if (in->op == NYIR_ADD_I64) {
    int r = find_alloc_root_depth(f, defs, in->a, depth + 1);
    if (r >= 0)
      return r;
    return find_alloc_root_depth(f, defs, in->b, depth + 1);
  }
  if (in->op == NYIR_ALLOCA)
    return v;
  if (in->op == NYIR_CALL && in->symbol &&
      (strcmp(in->symbol, "rt_tbuf_new_raw") == 0 ||
       strcmp(in->symbol, "malloc") == 0 ||
       strcmp(in->symbol, "calloc") == 0)) {
    return v;
  }
  return -1;
}

static int find_alloc_root(const nyr_func_t *f, const int *defs, int v) {
  return find_alloc_root_depth(f, defs, v, 0);
}

bool nyr_dead_store_elim(nyr_func_t *f) {
  if (!f || f->next_value <= 0)
    return true;
  bool trace = ny_dbg_is_enabled(NY_DBG_HEURISTIC);
  size_t local_candidates = 0;
  size_t same_block_removed = 0;
  size_t cfg_removed = 0;
  size_t escaped_barriers = 0;
  size_t allocation_candidates = 0;
  size_t dead_allocation_removed = 0;
  size_t count = nyr_max_local(f);
  if (!count)
    return true;

  unsigned *pending_epoch = (unsigned *)calloc(count, sizeof(*pending_epoch));
  bool *observed = (bool *)calloc(count, sizeof(*observed));
  bool *escaped = (bool *)calloc(count, sizeof(*escaped));
  if (!pending_epoch || !observed || !escaped) {
    free(pending_epoch);
    free(observed);
    free(escaped);
    return false;
  }
  /*
   * Direct local facts stay block-local. The CFG proof below extends them
   * only when all continuations are known not to observe the store.
   */
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if ((in->op == NYIR_LOAD_LOCAL || in->op == NYIR_ADDR_LOCAL) &&
        in->imm >= 0 && (size_t)in->imm < count)
      observed[in->imm] = true;
    if (in->op == NYIR_ADDR_LOCAL && in->imm >= 0 && (size_t)in->imm < count)
      escaped[in->imm] = true;
  }
  unsigned epoch = 1;
  for (size_t i = f->len; i-- > 0;) {
    nyr_inst_t *in = &f->data[i];
    if (in->op == NYIR_LABEL || in->op == NYIR_BR || in->op == NYIR_BR_IF ||
        in->op == NYIR_RET) {
      /*
       * Do not propagate facts across a control-flow boundary.
       */
      if (++epoch == 0) {
        memset(pending_epoch, 0, count * sizeof(*pending_epoch));
        epoch = 1;
      }
      continue;
    }
    if (in->op == NYIR_CALL) {
      if (++epoch == 0) {
        memset(pending_epoch, 0, count * sizeof(*pending_epoch));
        epoch = 1;
      }
      continue;
    }
    if (in->imm < 0 || (size_t)in->imm >= count)
      continue;
    size_t slot = (size_t)in->imm;
    if (in->op == NYIR_LOAD_LOCAL || in->op == NYIR_ADDR_LOCAL) {
      pending_epoch[slot] = 0;
      continue;
    }
    if (in->op != NYIR_STORE_LOCAL)
      continue;
    if (trace)
      local_candidates++;
    if (escaped[slot]) {
      if (trace)
        escaped_barriers++;
      pending_epoch[slot] = 0;
      continue;
    }
    if (pending_epoch[slot] == epoch) {
      if (trace)
        same_block_removed++;
      nyr_inst_discard(in);
    }
    pending_epoch[slot] = epoch;
  }
  nyr_cfg_t cfg = {0};
  if (nyr_cfg_build_topology(f, &cfg)) {
    for (size_t i = 0; i < f->len; ++i) {
      nyr_inst_t *in = &f->data[i];
      if (in->op != NYIR_STORE_LOCAL || in->imm < 0 ||
          (size_t)in->imm >= count || escaped[in->imm])
        continue;
      size_t block = cfg.inst_block[i];
      if (block < cfg.block_count && cfg.reachable[block] &&
          dse_store_dead_on_all_paths(f, &cfg, (size_t)in->imm, i)) {
        if (trace)
          cfg_removed++;
        nyr_inst_discard(in);
      }
    }
    nyr_cfg_free(&cfg);
  }
  int *defs = nyr_build_defs(f);
  if (defs) {
    bool *alloc_escapes = (bool *)calloc((size_t)f->next_value, sizeof(bool));
    bool *alloc_read = (bool *)calloc((size_t)f->next_value, sizeof(bool));
    if (alloc_escapes && alloc_read) {
      /*
       * Pass 1: find all reads, escapes, and parameter passes.
       */
      for (size_t i = 0; i < f->len; ++i) {
        const nyr_inst_t *in = &f->data[i];
        bool vec_load =
            in->op == NYIR_VEC4_LOAD_F64 || in->op == NYIR_VEC8_LOAD_F32 ||
            in->op == NYIR_VEC4_LOAD_I64 || in->op == NYIR_VEC8_LOAD_I64;
        if (in->op == NYIR_LOAD_I64 || vec_load) {
          int root = find_alloc_root(f, defs, in->a);
          if (root >= 0 && root < f->next_value)
            alloc_read[root] = true;
        }
        if (in->op == NYIR_COPY_STRUCT && in->b >= 0) {
          /*
           * COPY_STRUCT reads its source through .b (backend: rsi <- b).
           */
          int root = find_alloc_root(f, defs, in->b);
          if (root >= 0 && root < f->next_value)
            alloc_read[root] = true;
        }
        if (in->op == NYIR_RET && in->a >= 0) {
          int root = find_alloc_root(f, defs, in->a);
          if (root >= 0 && root < f->next_value)
            alloc_escapes[root] = true;
        }
        if (in->op == NYIR_CALL) {
          int args[6] = {in->a, in->b, in->c, in->d, in->e, in->f};
          for (int k = 0; k < 6; ++k) {
            if (args[k] >= 0) {
              int root = find_alloc_root(f, defs, args[k]);
              if (root >= 0 && root < f->next_value && root != in->dst)
                alloc_escapes[root] = true;
            }
          }
          /*
           * Expanded stack arguments are part of the same call boundary.
           * Missing storage for a nonempty vector means the IR is malformed;
           * conservatively keep every allocation store rather than discard
           * data that the callee might observe.
           */
          if (in->extra_args_len && !in->extra_args) {
            memset(alloc_escapes, 1,
                   (size_t)f->next_value * sizeof(*alloc_escapes));
          } else {
            for (size_t k = 0; k < in->extra_args_len; ++k) {
              int root = find_alloc_root(f, defs, in->extra_args[k]);
              if (root >= 0 && root < f->next_value && root != in->dst)
                alloc_escapes[root] = true;
            }
          }
        }
        if (in->op == NYIR_STORE_LOCAL && in->imm >= 0 &&
            (size_t)in->imm < count) {
          int root = find_alloc_root(f, defs, in->a);
          if (root >= 0 && root < f->next_value) {
            if (escaped[in->imm] || observed[in->imm])
              alloc_read[root] = true;
            if (escaped[in->imm])
              alloc_escapes[root] = true;
          }
        }
      }
      /*
       * Pass 2: discard stores to dead allocations with no reads or escapes.
       */
      for (size_t i = 0; i < f->len; ++i) {
        nyr_inst_t *in = &f->data[i];
        if (in->op == NYIR_STORE_I64 && in->a >= 0) {
          if (trace)
            allocation_candidates++;
          int root = find_alloc_root(f, defs, in->a);
          if (root >= 0 && root < f->next_value && !alloc_escapes[root] &&
              !alloc_read[root]) {
            if (trace)
              dead_allocation_removed++;
            nyr_inst_discard(in);
          }
        }
      }
    }
    free(alloc_escapes);
    free(alloc_read);
    free(defs);
  }
  free(pending_epoch);
  free(observed);
  free(escaped);
  if (trace)
    ny_dbg_heuristic_event(
        "DSE",
        "local candidates=%zu removed same-block=%zu all-paths=%zu "
        "escaped-barriers=%zu; allocation-store candidates=%zu removed "
        "dead-unescaped=%zu",
        local_candidates, same_block_removed, cfg_removed, escaped_barriers,
        allocation_candidates, dead_allocation_removed);
  return true;
}
