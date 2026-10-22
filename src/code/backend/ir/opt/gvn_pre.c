/*
 * GVN: global value numbering with dominance-checked redundancy
 * elimination (common-subexpression elimination) over SSA NYIR values.
 *
 * Each SSA value is assigned a value number by its defining operation and
 * operands.  Two instructions with the same value number compute the same
 * value; a redundant computation whose canonical (first) def dominates the
 * use is rewritten to a COPY of that def.
 *
 * The dominator relation comes from the CFG's packed-bitset dominators
 * (computed during nyr_cfg_build), not a local approximation.  When the
 * canonical def does not dominate the use, the computation is left in place
 * (a safe miss), so the transform never introduces an out-of-scope SSA use.
 *
 * Note: this is GVN, not partial-redundancy elimination — it does not do
 * code motion / anticipatability.
 */
#include "base/common.h"
#include "base/compat.h"
#include "base/debug.h"
#include "base/util.h"
#include "code/backend/ir/internal.h"
#include "code/backend/ir/opt/util.h"
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Hash table for value numbering.
 */
#define VN_TABLE_SIZE 4096

typedef struct {
  nyr_op_t op;
  int a, b, c, d, e, f;
  int64_t imm;
  nyr_cmp_t cmp;
  unsigned flags;
  unsigned effects;
  nyr_scalar_class_t scalar_class;
  uint8_t semantic_ownership;
  bool semantic_mutable;
  const char *symbol;
  const int *extra_args;
  size_t extra_args_len;
  const uint32_t *arg_sizes;
  uint64_t arg_ownership[NYIR_CALL_MAX_ARGS / 32];
  uint64_t arg_contracts[NYIR_CALL_MAX_ARGS / 32];
  int vn;  /* Value number */
  int dst; /* Defining SSA value */
  bool valid;
} vn_entry_t;

/*
 * GVN only admits operations whose result is a deterministic function of the
 * key below. Loads participate only when a dominance or merge-path memory
 * proof establishes that their addressed memory cannot have changed. Local
 * memory reads, allocations, volatile operations, control flow, and unknown
 * calls are deliberately excluded. Audited pure calls are allowed and are
 * keyed by callee symbol as well as all in-register arguments; calls with
 * out-of-line arguments stay conservative.
 */
static bool gvn_can_number(const nyr_inst_t *in) {
  if (!in || in->dst < 0 || in->op == NYIR_PHI)
    return false;
  /*
   * An unclassified immediate can acquire its representation only from its
   * eventual use (for example, `true` crossing a raw-bool call slot). Do not
   * merge it with an equal word defined in another ABI context.
   */
  if ((in->op == NYIR_CONST_I64 || in->op == NYIR_CONST_F32 ||
       in->op == NYIR_CONST_F64) &&
      in->scalar_class == NYIR_SCALAR_UNKNOWN)
    return false;
  if (in->op == NYIR_CALL) {
    unsigned effects = nyr_effective_effects(in);
    const unsigned blockers =
        NYIR_EFFECT_WRITE_LOCAL | NYIR_EFFECT_WRITE_MEMORY |
        NYIR_EFFECT_ALLOCATION | NYIR_EFFECT_IO | NYIR_EFFECT_THREAD |
        NYIR_EFFECT_FFI | NYIR_EFFECT_FENV | NYIR_EFFECT_VOLATILE |
        NYIR_EFFECT_UNKNOWN_SIDE_EFFECT;
    /*
     * Deterministic read-only helpers can share a value number.  A later
     * dominance/path check proves memory stability before such a hit is
     * actually rewritten; MAY_TRAP is allowed because the dominating call is
     * necessarily executed on every path that reaches the redundant call.
     */
    return (effects & NYIR_EFFECT_CALL) != 0 && (effects & blockers) == 0;
  }
  if (in->op == NYIR_LOAD_I64)
    return in->a >= 0;
  if (in->op == NYIR_LOAD_LOCAL)
    return in->imm >= 0;
  return nyr_inst_effects(in) == NYIR_EFFECT_NONE;
}

static bool gvn_invalidates_read_memory(const nyr_inst_t *in) {
  if (!in)
    return false;
  unsigned effects = nyr_effective_effects(in);
  const unsigned invalidates =
      NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_ALLOCATION | NYIR_EFFECT_IO |
      NYIR_EFFECT_THREAD | NYIR_EFFECT_FFI | NYIR_EFFECT_VOLATILE |
      NYIR_EFFECT_UNKNOWN_SIDE_EFFECT;
  return (effects & invalidates) != 0;
}

/*
 * LOAD_I64 PRE does not make independent alias claims. It asks the shared
 * effective-effect analysis whether every intervening operation is harmless.
 * Explicit effects distinguish read-only calls from writing or unknown calls.
 */
typedef enum {
  GVN_ADDR_UNKNOWN = 0,
  GVN_ADDR_SYMBOL,
  GVN_ADDR_LOCAL,
  GVN_ADDR_ALLOCA,
} gvn_addr_kind_t;

typedef struct {
  gvn_addr_kind_t kind;
  int id;
  const char *symbol;
  int64_t offset;
} gvn_addr_root_t;

static bool gvn_const_value(const nyr_func_t *f, const int *defs, int value,
                            int64_t *out) {
  if (!f || !defs || !out || value < 0 || value >= f->next_value)
    return false;
  for (unsigned depth = 0; depth < 16; ++depth) {
    int di = defs[value];
    if (di < 0 || (size_t)di >= f->len)
      return false;
    const nyr_inst_t *in = &f->data[di];
    if (in->op == NYIR_COPY) {
      value = in->a;
      if (value < 0 || value >= f->next_value)
        return false;
      continue;
    }
    if (in->op != NYIR_CONST_I64)
      return false;
    *out = in->imm;
    return true;
  }
  return false;
}

static bool gvn_address_root(const nyr_func_t *f, const int *defs, int value,
                             unsigned depth, gvn_addr_root_t *out) {
  if (!f || !defs || !out || depth > 16 || value < 0 || value >= f->next_value)
    return false;
  int di = defs[value];
  if (di < 0 || (size_t)di >= f->len)
    return false;
  const nyr_inst_t *in = &f->data[di];
  if (in->op == NYIR_COPY)
    return gvn_address_root(f, defs, in->a, depth + 1, out);
  if (in->op == NYIR_ADDR_SYMBOL && in->symbol) {
    *out = (gvn_addr_root_t){
        .kind = GVN_ADDR_SYMBOL, .symbol = in->symbol, .offset = 0};
    return true;
  }
  if (in->op == NYIR_ADDR_LOCAL && in->imm >= 0 && in->imm <= INT_MAX) {
    *out = (gvn_addr_root_t){
        .kind = GVN_ADDR_LOCAL, .id = (int)in->imm, .offset = 0};
    return true;
  }
  if (in->op == NYIR_ALLOCA) {
    *out = (gvn_addr_root_t){.kind = GVN_ADDR_ALLOCA, .id = value, .offset = 0};
    return true;
  }
  if (in->op == NYIR_ADD_I64 || in->op == NYIR_SUB_I64) {
    gvn_addr_root_t root;
    int64_t offset = 0;
    if (gvn_address_root(f, defs, in->a, depth + 1, &root) &&
        gvn_const_value(f, defs, in->b, &offset)) {
      int64_t adjusted;
      bool overflow =
          in->op == NYIR_ADD_I64
              ? __builtin_add_overflow(root.offset, offset, &adjusted)
              : __builtin_sub_overflow(root.offset, offset, &adjusted);
      if (!overflow) {
        root.offset = adjusted;
        *out = root;
        return true;
      }
    }
    if (in->op == NYIR_ADD_I64 &&
        gvn_address_root(f, defs, in->b, depth + 1, &root) &&
        gvn_const_value(f, defs, in->a, &offset)) {
      int64_t adjusted;
      if (!__builtin_add_overflow(root.offset, offset, &adjusted)) {
        root.offset = adjusted;
        *out = root;
        return true;
      }
    }
  }
  return false;
}

static bool gvn_ranges_disjoint(const nyr_func_t *f, const int *defs,
                                int read_address, int write_address,
                                uint64_t write_size) {
  gvn_addr_root_t read_root = {0}, write_root = {0};
  if (!gvn_address_root(f, defs, read_address, 0, &read_root) ||
      !gvn_address_root(f, defs, write_address, 0, &write_root))
    return false;
  if (read_root.kind != write_root.kind)
    return read_root.offset == 0 && write_root.offset == 0;
  switch (read_root.kind) {
  case GVN_ADDR_SYMBOL:
    if (!read_root.symbol || !write_root.symbol)
      return false;
    if (strcmp(read_root.symbol, write_root.symbol) != 0)
      return read_root.offset == 0 && write_root.offset == 0;
    break;
  case GVN_ADDR_LOCAL:
  case GVN_ADDR_ALLOCA:
    if (read_root.id != write_root.id)
      return read_root.offset == 0 && write_root.offset == 0;
    break;
  default:
    return false;
  }
  if (write_size == 0 || write_size > INT64_MAX)
    return false;
  int64_t read_end, write_end;
  if (__builtin_add_overflow(read_root.offset, (int64_t)sizeof(int64_t),
                             &read_end) ||
      __builtin_add_overflow(write_root.offset, (int64_t)write_size,
                             &write_end))
    return false;
  return write_end <= read_root.offset || read_end <= write_root.offset;
}

static bool gvn_call_write_range(const nyr_func_t *f, const int *defs,
                                 const nyr_inst_t *in, int read_address) {
  /*
   * These bridges have a fixed contract: `a` is the destination and `c` is
   * the byte count. A constant non-positive count writes nothing; a positive
   * count is usable only with provenance and a non-overlapping byte range.
   * All other calls and unknown lengths remain global memory barriers.
   */
  if (!in || in->op != NYIR_CALL || in->imm != 3 || !in->symbol ||
      !in->symbol[0])
    return false;
  if (strcmp(in->symbol, "rt_memcpy_raw") != 0 &&
      strcmp(in->symbol, "rt_memset_raw") != 0)
    return false;
  int64_t length = 0;
  if (!gvn_const_value(f, defs, in->c, &length))
    return false;
  if (length <= 0)
    return true;
  return gvn_ranges_disjoint(f, defs, read_address, in->a, (uint64_t)length);
}

static bool gvn_invalidates_load_memory(const nyr_func_t *f, const int *defs,
                                        const nyr_inst_t *in,
                                        bool local_writes_can_alias,
                                        int64_t local_slot,
                                        bool local_slot_escaped,
                                        int read_address) {
  if (!in)
    return false;
  if (in->op == NYIR_STORE_I64 && read_address >= 0 &&
      gvn_ranges_disjoint(f, defs, read_address, in->a, sizeof(int64_t)))
    return false;
  if (in->op == NYIR_CALL &&
      (nyr_effective_effects(in) & NYIR_EFFECT_WRITE_MEMORY) != 0 &&
      gvn_call_write_range(f, defs, in, read_address))
    return false;
  unsigned effects = nyr_effective_effects(in);
  if (local_slot >= 0) {
    if (in->op == NYIR_STORE_LOCAL)
      return in->imm == local_slot;
    if (effects & NYIR_EFFECT_WRITE_LOCAL)
      return true;
    if (!local_slot_escaped)
      return (effects &
              (NYIR_EFFECT_THREAD | NYIR_EFFECT_UNKNOWN_SIDE_EFFECT)) != 0;
  }
  const unsigned invalidates = NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_THREAD |
                               NYIR_EFFECT_FFI |
                               NYIR_EFFECT_UNKNOWN_SIDE_EFFECT;
  /*
   * A write-argument summary identifies where a call may start writing, not
   * how far it can write. Raw memcpy/memset lengths are runtime values, and a
   * different symbolic base can still have a range that overlaps this load.
   * Keep the call as a barrier until the extent is proven within a bounded,
   * non-aliasing object.
   */
  if (effects & NYIR_EFFECT_ALLOCATION) {
    gvn_addr_root_t root = {0};
    /*
     * Fresh allocation cannot change a static symbol cell or stack storage.
     * It can affect the lifetime of heap objects reached through an unknown
     * pointer, so retain the barrier unless the read has explicit provenance.
     */
    if (!gvn_address_root(f, defs, read_address, 0, &root))
      return true;
  }
  return (effects & invalidates) != 0 ||
         (local_writes_can_alias && (effects & NYIR_EFFECT_WRITE_LOCAL) != 0);
}

/*
 * Return a known local slot, -2 for a known non-local base, or -1 when the
 * address may alias either class. Preserve only explicit address provenance;
 * loaded pointers and PHIs remain unknown.
 */
static int gvn_address_local_slot(const nyr_func_t *f, const int *defs,
                                  int value) {
  if (!f || !defs)
    return -1;
  int current = value;
  for (unsigned depth = 0;
       depth < 16 && current >= 0 && current < f->next_value; ++depth) {
    int def_index = defs[current];
    if (def_index < 0 || (size_t)def_index >= f->len)
      return -1;
    const nyr_inst_t *def = &f->data[(size_t)def_index];
    if (def->op == NYIR_ADDR_LOCAL)
      return def->imm >= 0 && def->imm <= INT_MAX ? (int)def->imm : -1;
    if (def->op == NYIR_ADDR_SYMBOL || def->op == NYIR_ALLOCA)
      return -2;
    if (def->op == NYIR_COPY) {
      current = def->a;
      continue;
    }
    if (def->op == NYIR_ADD_I64 || def->op == NYIR_SUB_I64) {
      current = def->a;
      continue;
    }
    return -1;
  }
  return -1;
}

static bool gvn_backedge(const nyr_cfg_t *cfg, size_t source, size_t target) {
  return cfg && target < cfg->block_count && source < cfg->block_count &&
         nyr_cfg_dominates(cfg, target, source);
}

/*
 * A load can be stable on the path to its first use and still become stale
 * before the same static use executes again. This happens when the use is in
 * a loop and a later instruction in that loop writes the loaded location.
 * Check every path from just after the use back to the use, stopping at the
 * defining block because that load executes again before the value is reused.
 */
static bool gvn_load_stable_on_use_reentry(
    const nyr_func_t *f, const nyr_cfg_t *cfg, size_t use_at,
    size_t def_block, bool local_writes_can_alias, int64_t local_slot,
    bool local_slot_escaped, const int *defs, int read_address) {
  if (!f || !cfg || use_at >= f->len)
    return false;
  const size_t use_block = cfg->inst_block[use_at];
  if (use_block >= cfg->block_count || use_block == def_block)
    return true;

  bool in_loop = false;
  for (size_t source = 0; source < cfg->block_count && !in_loop; ++source) {
    for (size_t e = cfg->succ_offsets[source];
         e < cfg->succ_offsets[source + 1]; ++e) {
      size_t header = cfg->succ_blocks[e];
      if (gvn_backedge(cfg, source, header) &&
          nyr_cfg_dominates(cfg, header, use_block)) {
        in_loop = true;
        break;
      }
    }
  }
  if (!in_loop)
    return true;

  bool *can_reach_use = calloc(cfg->block_count, sizeof(*can_reach_use));
  bool *seen = calloc(cfg->block_count, sizeof(*seen));
  size_t *queue = malloc(cfg->block_count * sizeof(*queue));
  if (!can_reach_use || !seen || !queue) {
    free(can_reach_use);
    free(seen);
    free(queue);
    return false;
  }

  size_t head = 0, tail = 0;
  can_reach_use[use_block] = true;
  queue[tail++] = use_block;
  while (head < tail) {
    size_t block = queue[head++];
    for (size_t e = cfg->pred_offsets[block]; e < cfg->pred_offsets[block + 1];
         ++e) {
      size_t pred = cfg->pred_blocks[e];
      if (pred == def_block || can_reach_use[pred])
        continue;
      can_reach_use[pred] = true;
      queue[tail++] = pred;
    }
  }

  bool stable = true;
  head = tail = 0;
  for (size_t e = cfg->succ_offsets[use_block];
       e < cfg->succ_offsets[use_block + 1]; ++e) {
    size_t succ = cfg->succ_blocks[e];
    if (succ == def_block || !can_reach_use[succ] || seen[succ])
      continue;
    seen[succ] = true;
    queue[tail++] = succ;
  }
  if (tail == 0)
    goto done;
  size_t use_end = cfg->block_end[use_block];
  for (size_t i = use_at + 1; i < use_end; ++i) {
    if (gvn_invalidates_load_memory(f, defs, &f->data[i],
                                    local_writes_can_alias, local_slot,
                                    local_slot_escaped, read_address)) {
      stable = false;
      goto done;
    }
  }

  while (head < tail && stable) {
    size_t block = queue[head++];
    size_t begin = cfg->block_start[block];
    size_t end = cfg->block_end[block];
    if (block == def_block)
      continue;
    if (block == use_block) {
      for (size_t i = begin; i < use_at; ++i) {
        if (gvn_invalidates_load_memory(f, defs, &f->data[i],
                                        local_writes_can_alias, local_slot,
                                        local_slot_escaped, read_address)) {
          stable = false;
          break;
        }
      }
      for (size_t i = use_at + 1; stable && i < end; ++i) {
        if (gvn_invalidates_load_memory(f, defs, &f->data[i],
                                        local_writes_can_alias, local_slot,
                                        local_slot_escaped, read_address))
          stable = false;
      }
    } else {
      for (size_t i = begin; i < end; ++i) {
        if (gvn_invalidates_load_memory(f, defs, &f->data[i],
                                        local_writes_can_alias, local_slot,
                                        local_slot_escaped, read_address)) {
          stable = false;
          break;
        }
      }
    }
    if (!stable)
      break;
    for (size_t e = cfg->succ_offsets[block]; e < cfg->succ_offsets[block + 1];
         ++e) {
      size_t succ = cfg->succ_blocks[e];
      if (succ == def_block || !can_reach_use[succ] || seen[succ])
        continue;
      seen[succ] = true;
      queue[tail++] = succ;
    }
  }

done:
  free(can_reach_use);
  free(seen);
  free(queue);
  return stable;
}

/*
 * A load in a loop may be reused after a backedge only if every path from the
 * loop header to the use executes the defining block again.  Search one
 * iteration at a time: backedges are cut, and the defining block terminates
 * each path.  This avoids treating a previous iteration's load as the
 * current one when a branch can skip the definition.
 */
static bool gvn_loop_reexecutes_def(const nyr_cfg_t *cfg, size_t header,
                                    size_t def_block, size_t use_block) {
  if (!cfg || header >= cfg->block_count || def_block >= cfg->block_count ||
      use_block >= cfg->block_count)
    return false;
  if (header == def_block)
    return true;
  bool *seen = calloc(cfg->block_count, sizeof(*seen));
  size_t *queue = malloc(cfg->block_count * sizeof(*queue));
  if (!seen || !queue) {
    free(seen);
    free(queue);
    return false;
  }
  size_t head = 0, tail = 0;
  seen[header] = true;
  queue[tail++] = header;
  bool reaches_use = false;
  while (head < tail) {
    size_t block = queue[head++];
    if (block == def_block)
      continue;
    if (block == use_block) {
      reaches_use = true;
      break;
    }
    for (size_t e = cfg->succ_offsets[block]; e < cfg->succ_offsets[block + 1];
         ++e) {
      size_t succ = cfg->succ_blocks[e];
      if (gvn_backedge(cfg, block, succ) || seen[succ])
        continue;
      seen[succ] = true;
      queue[tail++] = succ;
    }
  }
  free(seen);
  free(queue);
  return !reaches_use;
}

/*
 * Prove that every CFG path from a dominating read-only definition to a
 * redundant use is free of memory-invalidating effects. For LOAD_I64, calls
 * are also barriers: only an unchanged memory version on every path permits
 * replacement. This is deliberately stronger than merely checking blocks that
 * dominate the use: a write on only one arm of a diamond must still invalidate
 * the reuse. A backedge is ignored only when the definition is guaranteed to
 * execute again before this use.
 */
static bool gvn_memory_stable_between(const nyr_func_t *f,
                                      const nyr_cfg_t *cfg, size_t def_at,
                                      size_t use_at, bool load_read,
                                      bool local_writes_can_alias,
                                      int64_t local_slot,
                                      bool local_slot_escaped, const int *defs,
                                      int read_address, size_t *barrier_at) {
  if (barrier_at)
    *barrier_at = SIZE_MAX;
  if (!f || !cfg || def_at >= f->len || use_at >= f->len || def_at >= use_at)
    return false;
  size_t def_block = cfg->inst_block[def_at];
  size_t use_block = cfg->inst_block[use_at];
  if (def_block >= cfg->block_count || use_block >= cfg->block_count)
    return false;
  if (def_block == use_block) {
    for (size_t i = def_at + 1; i < use_at; ++i)
      if (load_read ? gvn_invalidates_load_memory(
                          f, defs, &f->data[i], local_writes_can_alias,
                          local_slot, local_slot_escaped, read_address)
                    : gvn_invalidates_read_memory(&f->data[i])) {
        if (barrier_at)
          *barrier_at = i;
        return false;
      }
    return true;
  }
  for (size_t i = def_at + 1; i < cfg->block_end[def_block]; ++i)
    if (load_read ? gvn_invalidates_load_memory(
                        f, defs, &f->data[i], local_writes_can_alias,
                        local_slot, local_slot_escaped, read_address)
                  : gvn_invalidates_read_memory(&f->data[i])) {
      if (barrier_at)
        *barrier_at = i;
      return false;
    }

  bool *seen = calloc(cfg->block_count, sizeof(*seen));
  bool *can_reach_use = calloc(cfg->block_count, sizeof(*can_reach_use));
  size_t *queue = malloc(cfg->block_count * sizeof(*queue));
  bool *safe_backedge_header =
      calloc(cfg->block_count, sizeof(*safe_backedge_header));
  if (!seen || !can_reach_use || !queue || !safe_backedge_header) {
    free(seen);
    free(can_reach_use);
    free(queue);
    free(safe_backedge_header);
    return false;
  }
  /*
   * A cycle that returns to the defining block reexecutes the load before any
   * later instruction in that block. Other loop cycles remain in the
   * memory-effect walk, so writes on repeated iterations still invalidate a
   * load defined outside the loop.
   */
  for (size_t source = 0; source < cfg->block_count; ++source) {
    for (size_t e = cfg->succ_offsets[source];
         e < cfg->succ_offsets[source + 1]; ++e) {
      size_t header = cfg->succ_blocks[e];
      if (gvn_backedge(cfg, source, header) &&
          nyr_cfg_dominates(cfg, header, def_block) &&
          gvn_loop_reexecutes_def(cfg, header, def_block, use_block))
        safe_backedge_header[header] = true;
    }
  }
  size_t head = 0, tail = 0;
  /*
   * Restrict the memory proof to paths that reach this use in the same
   * dynamic execution of the defining block. Paths that only reach a loop
   * backedge are checked again from the next execution of the definition.
   */
  can_reach_use[use_block] = true;
  queue[tail++] = use_block;
  while (head < tail) {
    size_t block = queue[head++];
    for (size_t e = cfg->pred_offsets[block]; e < cfg->pred_offsets[block + 1];
         ++e) {
      size_t pred = cfg->pred_blocks[e];
      if ((gvn_backedge(cfg, pred, block) && safe_backedge_header[block]) ||
          can_reach_use[pred])
        continue;
      can_reach_use[pred] = true;
      queue[tail++] = pred;
    }
  }
  head = tail = 0;
  for (size_t e = cfg->succ_offsets[def_block];
       e < cfg->succ_offsets[def_block + 1]; ++e) {
    size_t succ = cfg->succ_blocks[e];
    if (!can_reach_use[succ])
      continue;
    if (succ == def_block)
      continue;
    if (gvn_backedge(cfg, def_block, succ) && safe_backedge_header[succ])
      continue;
    if (!seen[succ]) {
      seen[succ] = true;
      queue[tail++] = succ;
    }
  }
  bool reached_use = false;
  while (head < tail) {
    size_t block = queue[head++];
    /*
     * Re-entering this block executes the defining load again before the
     * use (both are ordered within the same basic block). The block's
     * post-definition effects were checked on its first visit.
     */
    if (block == def_block)
      continue;
    size_t begin = cfg->block_start[block];
    size_t end = block == use_block ? use_at : cfg->block_end[block];
    for (size_t i = begin; i < end; ++i) {
      if (load_read ? gvn_invalidates_load_memory(
                          f, defs, &f->data[i], local_writes_can_alias,
                          local_slot, local_slot_escaped, read_address)
                    : gvn_invalidates_read_memory(&f->data[i])) {
        if (barrier_at)
          *barrier_at = i;
        free(seen);
        free(can_reach_use);
        free(queue);
        free(safe_backedge_header);
        return false;
      }
    }
    if (block == use_block) {
      reached_use = true;
      continue;
    }
    for (size_t e = cfg->succ_offsets[block]; e < cfg->succ_offsets[block + 1];
         ++e) {
      size_t succ = cfg->succ_blocks[e];
      if (!can_reach_use[succ])
        continue;
      if (succ == def_block)
        continue;
      if (gvn_backedge(cfg, block, succ) && safe_backedge_header[succ])
        continue;
      if (!seen[succ]) {
        seen[succ] = true;
        queue[tail++] = succ;
      }
    }
  }
  free(seen);
  free(can_reach_use);
  free(queue);
  free(safe_backedge_header);
  return reached_use &&
         (!load_read ||
          gvn_load_stable_on_use_reentry(
              f, cfg, use_at, def_block, local_writes_can_alias, local_slot,
              local_slot_escaped, defs, read_address));
}

/*
 * A merge-header load may become a PHI only when every incoming edge already
 * computes the same load and each such load stays valid to this header. The
 * PHI preserves the original SSA destination, so no code motion or new load is
 * introduced.
 */
static int gvn_predecessor_load(const nyr_func_t *f, const nyr_cfg_t *cfg,
                                size_t pred_block, const nyr_inst_t *load,
                                size_t use_at, bool local_writes_can_alias,
                                int64_t local_slot, const int *defs) {
  for (size_t i = cfg->block_end[pred_block]; i > cfg->block_start[pred_block];
       --i) {
    const nyr_inst_t *candidate = &f->data[i - 1];
    if (candidate->op != NYIR_LOAD_I64 || candidate->dst < 0 ||
        candidate->a != load->a || candidate->flags != load->flags)
      continue;
    if (gvn_memory_stable_between(f, cfg, i - 1, use_at, true,
                                  local_writes_can_alias, local_slot, false,
                                  defs, load->a, NULL))
      return candidate->dst;
  }
  return -1;
}

static bool gvn_load_pre(nyr_func_t *f, const nyr_cfg_t *cfg, size_t at,
                         bool local_writes_can_alias, int64_t local_slot,
                         const int *defs) {
  nyr_inst_t *load = &f->data[at];
  size_t block = cfg->inst_block[at];
  if (block >= cfg->block_count)
    return false;
  for (size_t i = cfg->block_start[block]; i < at; ++i) {
    nyr_op_t op = f->data[i].op;
    if (op != NYIR_LABEL && op != NYIR_PHI && op != NYIR_NOP)
      return false;
  }
  size_t first = cfg->pred_offsets[block];
  size_t last = cfg->pred_offsets[block + 1];
  if (last - first < 2)
    return false;
  nyr_phi_incoming_t *incoming = calloc(last - first, sizeof(*incoming));
  if (!incoming)
    return false;
  for (size_t edge = first; edge < last; ++edge) {
    size_t pred = cfg->pred_blocks[edge];
    int value = gvn_predecessor_load(f, cfg, pred, load, at,
                                     local_writes_can_alias, local_slot, defs);
    if (value < 0) {
      free(incoming);
      return false;
    }
    incoming[edge - first] = (nyr_phi_incoming_t){
        .predecessor_label = cfg->block_label[pred], .value = value};
  }
  nyr_debug_loc_t debug = load->debug;
  nyr_range_t range = load->range;
  int dst = load->dst;
  nyr_inst_discard(load);
  *load = (nyr_inst_t){.op = NYIR_PHI,
                        .dst = dst,
                        .a = -1,
                        .b = -1,
                        .c = -1,
                        .d = -1,
                        .e = -1,
                        .f = -1,
                        .debug = debug,
                        .range = range,
                        .phi_incoming = incoming,
                        .phi_incoming_len = last - first};
  return true;
}

static uint64_t vn_hash_string(const char *s) {
  uint64_t h = UINT64_C(1469598103934665603);
  if (!s)
    return h;
  while (*s) {
    h ^= (unsigned char)*s++;
    h *= UINT64_C(1099511628211);
  }
  return h;
}

static bool vn_symbol_equal(const char *a, const char *b) {
  if (a == b)
    return true;
  if (!a || !b)
    return false;
  return strcmp(a, b) == 0;
}

/*
 * Initialize VN table.
 */
static void vn_table_init(vn_entry_t *table) {
  for (size_t i = 0; i < VN_TABLE_SIZE; ++i) {
    table[i].valid = false;
    table[i].vn = -1;
    table[i].dst = -1;
  }
}

/*
 * Compute a hash over every field that can change the semantics of a
 * numberable NYIR instruction.  This intentionally includes debug-irrelevant
 * ABI flags (for example float return class on a pure helper call), because
 * they affect the produced machine value representation.
 */
static size_t vn_hash(const nyr_inst_t *in) {
  uint64_t h = (uint64_t)in->op + UINT64_C(0x9e3779b97f4a7c15);
#define VN_MIX(v)                                                              \
  do {                                                                         \
    h ^= (uint64_t)(uint32_t)((v) + 1) + UINT64_C(0x9e3779b97f4a7c15) +        \
         (h << 6) + (h >> 2);                                                  \
  } while (0)
  VN_MIX(in->a);
  VN_MIX(in->b);
  VN_MIX(in->c);
  VN_MIX(in->d);
  VN_MIX(in->e);
  VN_MIX(in->f);
  h ^= (uint64_t)in->imm + UINT64_C(0x9e3779b97f4a7c15) + (h << 6) + (h >> 2);
  VN_MIX((int)in->cmp);
  VN_MIX((int)in->flags);
  VN_MIX((int)nyr_effective_effects(in));
  VN_MIX((int)in->scalar_class);
  VN_MIX((int)in->semantic_ownership);
  VN_MIX((int)in->semantic_mutable);
  h ^= vn_hash_string(in->symbol) + UINT64_C(0x9e3779b97f4a7c15) + (h << 6) +
       (h >> 2);
  VN_MIX((int)in->extra_args_len);
  for (size_t k = 0; k < in->extra_args_len; ++k)
    VN_MIX(in->extra_args[k]);
  if (in->op == NYIR_CALL && in->imm > 0) {
    for (int64_t k = 0; k < in->imm; ++k)
      VN_MIX((int)(in->arg_sizes ? in->arg_sizes[k] : 0u));
  }
  if (in->op == NYIR_CALL) {
    for (size_t k = 0; k < NYIR_CALL_MAX_ARGS / 32; ++k) {
      h ^= in->arg_ownership[k] * UINT64_C(0x9e3779b97f4a7c15);
      h ^= in->arg_contracts[k] * UINT64_C(0xbf58476d1ce4e5b9);
    }
  }
#undef VN_MIX
  return (size_t)(h % VN_TABLE_SIZE);
}

static bool vn_same_key(const vn_entry_t *e, const nyr_inst_t *in) {
  if (!e || !in || e->op != in->op || e->a != in->a || e->b != in->b ||
      e->c != in->c || e->d != in->d || e->e != in->e || e->f != in->f ||
      e->imm != in->imm || e->cmp != in->cmp || e->flags != in->flags ||
      e->effects != nyr_effective_effects(in) ||
      e->scalar_class != in->scalar_class ||
      e->semantic_ownership != in->semantic_ownership ||
      e->semantic_mutable != in->semantic_mutable ||
      !vn_symbol_equal(e->symbol, in->symbol) ||
      e->extra_args_len != in->extra_args_len)
    return false;
  if (in->op == NYIR_CALL && (memcmp(e->arg_ownership, in->arg_ownership,
                                     sizeof(e->arg_ownership)) != 0 ||
                              memcmp(e->arg_contracts, in->arg_contracts,
                                     sizeof(e->arg_contracts)) != 0))
    return false;
  for (size_t k = 0; k < in->extra_args_len; ++k)
    if (!e->extra_args || e->extra_args[k] != in->extra_args[k])
      return false;
  if (in->op == NYIR_CALL && in->imm > 0) {
    for (int64_t k = 0; k < in->imm; ++k) {
      uint32_t left = e->arg_sizes ? e->arg_sizes[k] : 0u;
      uint32_t right = in->arg_sizes ? in->arg_sizes[k] : 0u;
      if (left != right)
        return false;
    }
  }
  return true;
}

/*
 * Lookup value number in table.
 */
static int vn_lookup(vn_entry_t *table, const nyr_inst_t *in) {
  size_t idx = vn_hash(in);
  for (size_t probe = 0; probe < VN_TABLE_SIZE; ++probe) {
    const vn_entry_t *e = &table[idx];
    if (!e->valid)
      return -1;
    if (vn_same_key(e, in))
      return e->vn;
    idx = (idx + 1) % VN_TABLE_SIZE;
  }
  return -1;
}

/*
 * Insert or update value number.
 */
static void vn_insert(vn_entry_t *table, const nyr_inst_t *in, int vn,
                      int dst) {
  size_t idx = vn_hash(in);
  for (size_t probe = 0; probe < VN_TABLE_SIZE; ++probe) {
    vn_entry_t *e = &table[idx];
    if (!e->valid || vn_same_key(e, in)) {
      if (!e->valid) {
        e->op = in->op;
        e->a = in->a;
        e->b = in->b;
        e->c = in->c;
        e->d = in->d;
        e->e = in->e;
        e->f = in->f;
        e->imm = in->imm;
        e->cmp = in->cmp;
        e->flags = in->flags;
        e->effects = nyr_effective_effects(in);
        e->scalar_class = in->scalar_class;
        e->semantic_ownership = in->semantic_ownership;
        e->semantic_mutable = in->semantic_mutable;
        e->symbol = in->symbol;
        e->extra_args = in->extra_args;
        e->extra_args_len = in->extra_args_len;
        e->arg_sizes = in->arg_sizes;
        memcpy(e->arg_ownership, in->arg_ownership, sizeof(e->arg_ownership));
        memcpy(e->arg_contracts, in->arg_contracts, sizeof(e->arg_contracts));
      }
      /*
       * The latest same-key def is the best candidate for following uses.
       */
      e->dst = dst;
      e->vn = vn;
      e->valid = true;
      return;
    }
    idx = (idx + 1) % VN_TABLE_SIZE;
  }
}

static int gvn_canonical_value(int value, const int *copy_of,
                               size_t value_count) {
  if (value < 0 || (size_t)value >= value_count)
    return value;
  return copy_of[value] >= 0 ? copy_of[value] : value;
}

static int gvn_canonical_operand(int value, const int *copy_of,
                                const int *constant_of, size_t value_count) {
  value = gvn_canonical_value(value, copy_of, value_count);
  if (value < 0 || (size_t)value >= value_count)
    return value;
  int canonical = constant_of[value];
  return canonical >= 0 ? canonical : value;
}

static void gvn_canonicalize_operands(nyr_inst_t *key,
                                      const int *copy_of,
                                      const int *constant_of,
                                      size_t value_count) {
#define GVN_CANONICALIZE_OPERAND(field)                                        \
  key->field = gvn_canonical_operand(key->field, copy_of, constant_of,         \
                                     value_count)
  GVN_CANONICALIZE_OPERAND(a);
  GVN_CANONICALIZE_OPERAND(b);
  GVN_CANONICALIZE_OPERAND(c);
  GVN_CANONICALIZE_OPERAND(d);
  GVN_CANONICALIZE_OPERAND(e);
  GVN_CANONICALIZE_OPERAND(f);
#undef GVN_CANONICALIZE_OPERAND
}

static bool gvn_pass(nyr_func_t *f, int *vn_of_val, nyr_cfg_t *cfg) {
  if (!f || f->next_value <= 0)
    return true;

  vn_entry_t table[VN_TABLE_SIZE];
  vn_table_init(table);

  /*
   * Direct local-slot stores cannot change an arbitrary memory load unless
   * the function has materialized a local-slot address. Keep the old broad
   * barrier in that case: such an address may have escaped and alias a later
   * load. Without ADDR_LOCAL, only memory effects can mutate LOAD_I64 data.
   */
  bool local_writes_can_alias = false;
  for (size_t i = 0; i < f->len; ++i)
    if (f->data[i].op == NYIR_ADDR_LOCAL) {
      local_writes_can_alias = true;
      break;
    }

  /*
   * value -> defining instruction index; vn -> canonical dst.  Both maps
   * make each redundant-hit check O(1) instead of a linear table/IR scan.
   */
  int *val_def = nyr_build_defs(f);
  if (!val_def)
    return true;
  int *vn_dst = malloc(((size_t)f->len + 1) * sizeof(*vn_dst));
  int *copy_of = malloc((size_t)f->next_value * sizeof(*copy_of));
  int *constant_of = malloc((size_t)f->next_value * sizeof(*constant_of));
  if (!vn_dst || !copy_of || !constant_of) {
    free(vn_dst);
    free(copy_of);
    free(constant_of);
    free(val_def);
    return true;
  }
  for (size_t j = 0; j < (size_t)f->len + 1; ++j)
    vn_dst[j] = -1;
  for (int value = 0; value < f->next_value; ++value)
    copy_of[value] = value;
  for (int value = 0; value < f->next_value; ++value)
    constant_of[value] = -1;

  /*
   * Ordinary GVN rewrites inside natural loops miscompile the FFT fixture at
   * O3. Keep those rewrites out until loop-carried value equivalence is
   * proven across the complete backedge path. Load-PRE has separate memory
   * and dominance checks and is handled above.
   */
  bool loop_analysis_failed = false;
  bool *loop_blocks = calloc(cfg->block_count, sizeof(*loop_blocks));
  bool *loop_members = calloc(cfg->block_count, sizeof(*loop_members));
  if (!loop_blocks || !loop_members) {
    loop_analysis_failed = true;
  } else {
    for (size_t source = 0; source < cfg->block_count; ++source) {
      for (size_t edge = cfg->succ_offsets[source];
           edge < cfg->succ_offsets[source + 1]; ++edge) {
        if (!cfg->backedge_edges[edge])
          continue;
        if (!nyr_cfg_natural_loop_blocks(cfg, source, cfg->succ_blocks[edge],
                                          loop_members,
                                          cfg->block_count)) {
          loop_analysis_failed = true;
          break;
        }
        for (size_t block = 0; block < cfg->block_count; ++block)
          loop_blocks[block] |= loop_members[block];
      }
      if (loop_analysis_failed)
        break;
    }
  }
  free(loop_members);

  /*
   * Equal literal values may canonicalize as operands of the same NYIR
   * operation even when their standalone scalar class is unknown.  The
   * operation key still carries its own opcode, flags and ABI metadata, so
   * this does not merge or rewrite the literal definitions themselves.
   */
  vn_entry_t constant_table[VN_TABLE_SIZE];
  vn_table_init(constant_table);
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->op != NYIR_CONST_I64 || in->dst < 0 ||
        in->dst >= f->next_value)
      continue;
    nyr_inst_t key = *in;
    key.scalar_class = in->scalar_class;
    int existing = vn_lookup(constant_table, &key);
    if (existing >= 0) {
      constant_of[in->dst] = existing;
    } else {
      int canonical = in->dst;
      vn_insert(constant_table, &key, canonical, canonical);
      constant_of[in->dst] = canonical;
    }
  }

  int next_vn = 0;
  bool trace = ny_dbg_is_enabled(NY_DBG_HEURISTIC);
  bool trace_misses = trace && ny_env_enabled("NY_TRACE_GVN_MISSES");
  bool trace_hits = trace && ny_env_enabled("NY_TRACE_GVN_HITS");
  size_t candidates = 0;
  size_t numbered = 0;
  size_t excluded_loop = 0;
  size_t excluded_effects = 0;
  size_t excluded_calls = 0;
  size_t dominance_misses = 0;
  size_t memory_misses = 0;
  size_t duplicates_replaced = 0;
  size_t loads_handled_by_pre = 0;
  for (size_t i = 0; i < f->len; ++i) {
    nyr_inst_t *in = &f->data[i];
    if (in->dst < 0 || in->dst >= (int)f->next_value)
      continue;
    /*
     * Copies preserve value identity, including copies of non-numbered PHIs.
     */
    if (in->op == NYIR_COPY && in->a >= 0 && in->a < f->next_value) {
      copy_of[in->dst] =
          gvn_canonical_value(in->a, copy_of, (size_t)f->next_value);
      if (vn_of_val[in->a] >= 0)
        vn_of_val[in->dst] = vn_of_val[in->a];
      continue;
    }
    int64_t load_local_slot = -1;
    bool load_local_writes_can_alias = local_writes_can_alias;
    if (in->op == NYIR_LOAD_LOCAL && in->imm >= 0) {
      load_local_slot = in->imm;
      load_local_writes_can_alias = false;
    } else if (in->op == NYIR_LOAD_I64) {
      int address_slot = gvn_address_local_slot(f, val_def, in->a);
      if (address_slot >= 0) {
        load_local_slot = address_slot;
        load_local_writes_can_alias = false;
      } else if (address_slot == -2) {
        load_local_writes_can_alias = false;
      }
    }
    if (in->op == NYIR_LOAD_I64 &&
        gvn_load_pre(f, cfg, i, load_local_writes_can_alias, load_local_slot,
                     val_def)) {
      if (trace)
        loads_handled_by_pre++;
      continue;
    }

    if (trace)
      candidates++;
    size_t block = cfg->inst_block[i];
    if (loop_analysis_failed || !loop_blocks ||
        (block < cfg->block_count && loop_blocks[block])) {
      if (trace)
        excluded_loop++;
      continue;
    }
    if (!gvn_can_number(in)) {
      if (trace) {
        if (in->op == NYIR_CALL)
          excluded_calls++;
        else
          excluded_effects++;
      }
      continue;
    }
    if (trace)
      numbered++;

    nyr_inst_t key = *in;
    gvn_canonicalize_operands(&key, copy_of, constant_of,
                              (size_t)f->next_value);
    if (trace_misses && in->op == NYIR_LOAD_I64) {
      int address_def = in->a >= 0 && in->a < f->next_value
                            ? val_def[in->a]
                            : -1;
      const nyr_inst_t *address =
          address_def >= 0 && (size_t)address_def < f->len
              ? &f->data[address_def]
              : NULL;
      ny_dbg_heuristic_event(
          "GVN_LOAD_KEY",
          "inst=%zu dst=v%d address=v%d key-address=%d vn-address=%d "
          "address-def=%d:%s a=v%d/vn%d b=v%d/vn%d numbered=%zu",
          i, in->dst, in->a, key.a,
          in->a >= 0 && in->a < f->next_value ? vn_of_val[in->a] : -1,
          address_def, address ? nyr_op_name(address->op) : "-",
          address ? address->a : -1,
          address && address->a >= 0 && address->a < f->next_value
              ? vn_of_val[address->a]
              : -1,
          address ? address->b : -1,
          address && address->b >= 0 && address->b < f->next_value
              ? vn_of_val[address->b]
              : -1,
          numbered);
    }
    /*
     * For non-call expressions, result facts normally belong to the replaced
     * SSA definition. nir_make_copy drops derived facts by default; this pass
     * restores an individual fact only after proving the expressions equal.
     * However, two *known and different* scalar classes are not equivalent:
     * equal words may be tagged immediates or raw integers. Keep a
     * class-specific key as a fallback when the neutral expression lookup
     * finds such an incompatible producer.
     */
    bool expression_key = key.op != NYIR_CALL;
    if (expression_key) {
      key.scalar_class = NYIR_SCALAR_UNKNOWN;
      key.semantic_ownership = NY_SEM_OWN_UNKNOWN;
      key.semantic_mutable = false;
    }
    int vn = vn_lookup(table, &key);
    if (trace_misses && in->op == NYIR_LOAD_I64 &&
        (in->flags & NYIR_INST_F_MEM_BYTE))
      ny_dbg_heuristic_event(
          "GVN_BYTE_KEY",
          "inst=i%zu value=v%d address=v%d canonical-address=v%d flags=0x%x "
          "effects=0x%x value-number=%d",
          i, in->dst, in->a, key.a, in->flags, nyr_effective_effects(in), vn);
    if (vn >= 0 && expression_key) {
      int candidate = vn < (int)f->len + 1 ? vn_dst[vn] : -1;
      int candidate_def =
          candidate >= 0 && candidate < f->next_value ? val_def[candidate] : -1;
      if (candidate_def >= 0 && (size_t)candidate_def < f->len) {
        const nyr_inst_t *producer = &f->data[candidate_def];
        bool class_conflict = producer->scalar_class != NYIR_SCALAR_UNKNOWN &&
                              in->scalar_class != NYIR_SCALAR_UNKNOWN &&
                              producer->scalar_class != in->scalar_class;
        bool ownership_conflict =
            producer->semantic_ownership != NY_SEM_OWN_UNKNOWN &&
            in->semantic_ownership != NY_SEM_OWN_UNKNOWN &&
            producer->semantic_ownership != in->semantic_ownership;
        if (class_conflict || ownership_conflict) {
          key.scalar_class = in->scalar_class;
          key.semantic_ownership = in->semantic_ownership;
          key.semantic_mutable = in->semantic_mutable;
          vn = vn_lookup(table, &key);
        }
      }
    }
    if (vn >= 0) {
      int cand = vn < (int)f->len + 1 ? vn_dst[vn] : -1;
      if (cand >= 0 && cand != in->dst && cand < f->next_value) {
        size_t def = (size_t)val_def[cand];
        if (def < f->len) {
          size_t def_block = cfg->inst_block[def];
          size_t use_block = cfg->inst_block[i];
          int64_t local_slot = in->op == NYIR_LOAD_LOCAL && in->imm >= 0
                                   ? in->imm
                               : in->op == NYIR_LOAD_I64 ? load_local_slot
                                                         : -1;
          bool dom = (def_block == use_block && def < i) ||
                     (def_block != use_block &&
                      nyr_cfg_dominates(cfg, def_block, use_block));
          if (dom) {
            bool reads_memory =
                (nyr_effective_effects(in) & NYIR_EFFECT_READ_MEMORY) != 0;
            bool local_read = in->op == NYIR_LOAD_LOCAL;
            size_t barrier_at = SIZE_MAX;
            bool stable =
                (!reads_memory && !local_read) ||
                gvn_memory_stable_between(
                    f, cfg, def, i, in->op == NYIR_LOAD_I64 || local_read,
                    load_local_writes_can_alias, local_slot,
                    local_read && local_slot >= 0 &&
                        load_local_writes_can_alias,
                    val_def, in->op == NYIR_LOAD_I64 ? in->a : -1,
                    trace_misses ? &barrier_at : NULL);
            if (stable) {
              if (trace)
                duplicates_replaced++;
              if (trace_hits)
                ny_dbg_heuristic_event(
                    "GVN_HIT",
                    "%s value=v%d replaced-with=v%d def=i%zu use=i%zu "
                    "blocks=B%zu→B%zu memory=%s",
                    nyr_op_name(in->op), in->dst, cand, def, i, def_block,
                    use_block, reads_memory ? "stable" : "pure");
              uint32_t value_alias = f->data[def].alias_class;
              nyr_range_t value_range = in->range;
              nir_make_copy(in, cand);
              /*
               * The equality and memory-stability proof above also justifies
               * carrying the replaced result's range onto this COPY.
               */
              in->range = value_range;
              /*
               * This instruction has already passed the COPY case at the
               * top of the scan. Record the replacement now so later keys
               * canonicalize through it in this same pass (for example,
               * identical address arithmetic feeding a load).
               */
              copy_of[in->dst] =
                  gvn_canonical_value(cand, copy_of, (size_t)f->next_value);
              if (vn_of_val[cand] >= 0)
                vn_of_val[in->dst] = vn_of_val[cand];
              /*
               * A forwarded pointer value follows the source value's
               * provenance, not the redundant load's address class.
               */
              in->alias_class = value_alias;
              continue;
            } else if (trace) {
              memory_misses++;
              if (trace_misses &&
                  (in->op == NYIR_LOAD_I64 || in->op == NYIR_LOAD_LOCAL)) {
                if (barrier_at < f->len) {
                  const nyr_inst_t *barrier = &f->data[barrier_at];
                  ny_dbg_heuristic_event(
                      "GVN_MISS",
                      "%s value=v%d candidate=v%d def=i%zu use=i%zu "
                      "blocks=B%zu→B%zu reason=memory-proof-failed "
                      "barrier=i%zu:%s callee=%s effects=0x%x address=v%d "
                      "alias=%u local-slot=%" PRId64,
                      nyr_op_name(in->op), in->dst, cand, def, i, def_block,
                      use_block, barrier_at, nyr_op_name(barrier->op),
                      barrier->symbol ? barrier->symbol : "-",
                      nyr_effective_effects(barrier), in->a, in->alias_class,
                      local_slot);
                } else {
                  ny_dbg_heuristic_event(
                      "GVN_MISS",
                      "%s value=v%d candidate=v%d def=i%zu use=i%zu "
                      "blocks=B%zu→B%zu reason=memory-proof-failed "
                      "barrier=control-flow address=v%d alias=%u "
                      "local-slot=%" PRId64,
                      nyr_op_name(in->op), in->dst, cand, def, i, def_block,
                      use_block, in->a, in->alias_class, local_slot);
                }
              }
            }
          } else if (trace) {
            dominance_misses++;
            if (trace_misses &&
                (in->op == NYIR_LOAD_I64 || in->op == NYIR_LOAD_LOCAL))
              ny_dbg_heuristic_event(
                  "GVN_MISS",
                  "%s value=v%d candidate=v%d def=i%zu use=i%zu "
                  "blocks=B%zu→B%zu reason=candidate-does-not-dominate "
                  "address=v%d alias=%u local-slot=%" PRId64,
                  nyr_op_name(in->op), in->dst, cand, def, i, def_block,
                  use_block, in->a, in->alias_class, local_slot);
          }
        }
      }
      /*
       * The prior same-key value may be stale across a memory effect or may
       * not dominate this path. Keep this definition as the new candidate:
       * later uses still prove dominance and memory stability before rewrite.
       */
      int new_vn = next_vn++;
      vn_insert(table, &key, new_vn, in->dst);
      if (new_vn < (int)f->len + 1)
        vn_dst[new_vn] = in->dst;
      if (in->dst >= 0)
        vn_of_val[in->dst] = new_vn;
    } else {
      int new_vn = next_vn++;
      vn_insert(table, &key, new_vn, in->dst);
      if (new_vn < (int)f->len + 1)
        vn_dst[new_vn] = in->dst;
      if (in->dst >= 0)
        vn_of_val[in->dst] = new_vn;
    }
  }
  if (trace)
    ny_dbg_heuristic_event(
        "GVN_PRE",
        "candidates=%zu numbered=%zu excluded-loop=%zu "
        "excluded-effectful=%zu excluded-calls=%zu load-PRE-handled=%zu "
        "duplicates-replaced=%zu dominance-misses=%zu memory-proof-misses=%zu",
        candidates, numbered, excluded_loop, excluded_effects, excluded_calls,
        loads_handled_by_pre, duplicates_replaced, dominance_misses,
        memory_misses);
  free(vn_dst);
  free(copy_of);
  free(constant_of);
  free(val_def);
  free(loop_blocks);
  return true;
}

bool nyr_gvn_pre(nyr_func_t *f) {
  if (!f || f->len < 2 || f->next_value <= 0)
    return true;

  nyr_cfg_t cfg = {0};
  if (!nyr_cfg_build(f, &cfg))
    return false;

  int *vn_of_val = calloc((size_t)f->next_value, sizeof(*vn_of_val));
  if (!vn_of_val) {
    nyr_cfg_free(&cfg);
    return false;
  }
  for (int value = 0; value < f->next_value; ++value)
    vn_of_val[value] = -1;
  gvn_pass(f, vn_of_val, &cfg);
  free(vn_of_val);
  nyr_cfg_free(&cfg);
  return true;
}
