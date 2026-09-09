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
 * (computed during nyir_cfg_build), not a local approximation.  When the
 * canonical def does not dominate the use, the computation is left in place
 * (a safe miss), so the transform never introduces an out-of-scope SSA use.
 *
 * Note: this is GVN, not partial-redundancy elimination — it does not do
 * code motion / anticipatability.
 */
#include "code/ir/opt/util.h"
#include "code/ir/internal.h"
#include "base/compat.h"
#include "base/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Hash table for value numbering.
 */
#define VN_TABLE_SIZE 4096

typedef struct {
  nyir_op_t op;
  int a, b, c, d, e, f;
  int64_t imm;
  nyir_cmp_t cmp;
  unsigned flags;
  const char *symbol;
  const int *extra_args;
  size_t extra_args_len;
  const uint32_t *arg_sizes;
  int vn;          /* Value number */
  int dst;         /* Defining SSA value */
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
static bool gvn_can_number(const nyir_inst_t *in) {
  if (!in || in->dst < 0 || in->op == NYIR_PHI)
    return false;
  if (in->op == NYIR_CALL) {
    unsigned effects = nyir_call_effect_summary(in);
    const unsigned blockers = NYIR_EFFECT_WRITE_LOCAL |
                              NYIR_EFFECT_WRITE_MEMORY |
                              NYIR_EFFECT_ALLOCATION |
                              NYIR_EFFECT_IO | NYIR_EFFECT_THREAD |
                              NYIR_EFFECT_FFI | NYIR_EFFECT_FENV |
                              NYIR_EFFECT_VOLATILE |
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
  return nyir_inst_effects(in) == NYIR_EFFECT_NONE;
}

static bool gvn_invalidates_read_memory(const nyir_inst_t *in) {
  if (!in)
    return false;
  unsigned effects = nyir_effective_effects(in);
  const unsigned invalidates = NYIR_EFFECT_WRITE_MEMORY |
                               NYIR_EFFECT_ALLOCATION | NYIR_EFFECT_IO |
                               NYIR_EFFECT_THREAD | NYIR_EFFECT_FFI |
                               NYIR_EFFECT_VOLATILE |
                               NYIR_EFFECT_UNKNOWN_SIDE_EFFECT;
  return (effects & invalidates) != 0;
}

/*
 * LOAD_I64 PRE does not make independent alias claims. It asks the shared
 * effective-effect analysis whether every intervening operation is harmless,
 * and treats every store and call as a barrier when no stronger memory version
 * fact is available.
 */
static bool gvn_invalidates_load_memory(const nyir_inst_t *in) {
  if (!in)
    return false;
  if (in->op == NYIR_CALL)
    return true;
  unsigned effects = nyir_effective_effects(in);
  const unsigned invalidates = NYIR_EFFECT_WRITE_LOCAL |
                               NYIR_EFFECT_WRITE_MEMORY |
                               NYIR_EFFECT_ALLOCATION | NYIR_EFFECT_IO |
                               NYIR_EFFECT_THREAD | NYIR_EFFECT_FFI |
                               NYIR_EFFECT_FENV | NYIR_EFFECT_VOLATILE |
                               NYIR_EFFECT_UNKNOWN_SIDE_EFFECT;
  return (effects & invalidates) != 0;
}

/*
 * Prove that every CFG path from a dominating read-only definition to a
 * redundant use is free of memory-invalidating effects. For LOAD_I64, calls
 * are also barriers: only an unchanged memory version on every path permits
 * replacement. This is deliberately stronger than merely checking blocks that
 * dominate the use: a write on only one arm of a diamond must still invalidate
 * the reuse. Re-entering the defining block is rejected conservatively so
 * loop-carried memory epochs never rely on an accidental static-dominance
 * interpretation.
 */
static bool gvn_memory_stable_between(const nyir_func_t *f,
                                      const nyir_cfg_t *cfg, size_t def_at,
                                      size_t use_at, bool load_read) {
  if (!f || !cfg || def_at >= f->len || use_at >= f->len || def_at >= use_at)
    return false;
  size_t def_block = cfg->inst_block[def_at];
  size_t use_block = cfg->inst_block[use_at];
  if (def_block >= cfg->block_count || use_block >= cfg->block_count)
    return false;
  if (def_block == use_block) {
    for (size_t i = def_at + 1; i < use_at; ++i)
      if (load_read ? gvn_invalidates_load_memory(&f->data[i])
                    : gvn_invalidates_read_memory(&f->data[i]))
        return false;
    return true;
  }
  for (size_t i = def_at + 1; i < cfg->block_end[def_block]; ++i)
    if (load_read ? gvn_invalidates_load_memory(&f->data[i])
                  : gvn_invalidates_read_memory(&f->data[i]))
      return false;

  bool *seen = calloc(cfg->block_count, sizeof(*seen));
  size_t *queue = malloc(cfg->block_count * sizeof(*queue));
  if (!seen || !queue) {
    free(seen);
    free(queue);
    return false;
  }
  size_t head = 0, tail = 0;
  for (size_t e = cfg->succ_offsets[def_block];
       e < cfg->succ_offsets[def_block + 1]; ++e) {
    size_t succ = cfg->succ_blocks[e];
    if (succ == def_block) {
      free(seen);
      free(queue);
      return false;
    }
    if (!seen[succ]) {
      seen[succ] = true;
      queue[tail++] = succ;
    }
  }
  bool reached_use = false;
  while (head < tail) {
    size_t block = queue[head++];
    if (block == def_block) {
      free(seen);
      free(queue);
      return false;
    }
    size_t begin = cfg->block_start[block];
    size_t end = block == use_block ? use_at : cfg->block_end[block];
    for (size_t i = begin; i < end; ++i) {
      if (load_read ? gvn_invalidates_load_memory(&f->data[i])
                    : gvn_invalidates_read_memory(&f->data[i])) {
        free(seen);
        free(queue);
        return false;
      }
    }
    if (block == use_block) {
      reached_use = true;
      continue;
    }
    for (size_t e = cfg->succ_offsets[block];
         e < cfg->succ_offsets[block + 1]; ++e) {
      size_t succ = cfg->succ_blocks[e];
      if (succ == def_block) {
        free(seen);
        free(queue);
        return false;
      }
      if (!seen[succ]) {
        seen[succ] = true;
        queue[tail++] = succ;
      }
    }
  }
  free(seen);
  free(queue);
  return reached_use;
}

/*
 * A merge-header load may become a PHI only when every incoming edge already
 * computes the same load and each such load stays valid to this header. The
 * PHI preserves the original SSA destination, so no code motion or new load is
 * introduced.
 */
static int gvn_predecessor_load(const nyir_func_t *f, const nyir_cfg_t *cfg,
                                size_t pred_block, const nyir_inst_t *load,
                                size_t use_at) {
  for (size_t i = cfg->block_end[pred_block];
       i > cfg->block_start[pred_block]; --i) {
    const nyir_inst_t *candidate = &f->data[i - 1];
    if (candidate->op != NYIR_LOAD_I64 || candidate->dst < 0 ||
        candidate->a != load->a || candidate->flags != load->flags)
      continue;
    if (gvn_memory_stable_between(f, cfg, i - 1, use_at, true))
      return candidate->dst;
  }
  return -1;
}

static bool gvn_load_pre(nyir_func_t *f, const nyir_cfg_t *cfg, size_t at) {
  nyir_inst_t *load = &f->data[at];
  size_t block = cfg->inst_block[at];
  if (block >= cfg->block_count)
    return false;
  for (size_t i = cfg->block_start[block]; i < at; ++i) {
    nyir_op_t op = f->data[i].op;
    if (op != NYIR_LABEL && op != NYIR_PHI && op != NYIR_NOP)
      return false;
  }
  size_t first = cfg->pred_offsets[block];
  size_t last = cfg->pred_offsets[block + 1];
  if (last - first < 2)
    return false;
  nyir_phi_incoming_t *incoming = calloc(last - first, sizeof(*incoming));
  if (!incoming)
    return false;
  for (size_t edge = first; edge < last; ++edge) {
    size_t pred = cfg->pred_blocks[edge];
    int value = gvn_predecessor_load(f, cfg, pred, load, at);
    if (value < 0) {
      free(incoming);
      return false;
    }
    incoming[edge - first] = (nyir_phi_incoming_t){
        .predecessor_label = cfg->block_label[pred], .value = value};
  }
  nyir_debug_loc_t debug = load->debug;
  nyir_range_t range = load->range;
  int dst = load->dst;
  nyir_inst_discard(load);
  *load = (nyir_inst_t){.op = NYIR_PHI, .dst = dst, .a = -1, .b = -1,
                         .c = -1, .d = -1, .e = -1, .f = -1,
                         .debug = debug, .range = range,
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
static size_t vn_hash(const nyir_inst_t *in) {
  uint64_t h = (uint64_t)in->op + UINT64_C(0x9e3779b97f4a7c15);
#define VN_MIX(v) do { \
    h ^= (uint64_t)(uint32_t)((v) + 1) + UINT64_C(0x9e3779b97f4a7c15) + \
         (h << 6) + (h >> 2); \
  } while (0)
  VN_MIX(in->a); VN_MIX(in->b); VN_MIX(in->c);
  VN_MIX(in->d); VN_MIX(in->e); VN_MIX(in->f);
  h ^= (uint64_t)in->imm + UINT64_C(0x9e3779b97f4a7c15) + (h << 6) + (h >> 2);
  VN_MIX((int)in->cmp);
  VN_MIX((int)in->flags);
  h ^= vn_hash_string(in->symbol) + UINT64_C(0x9e3779b97f4a7c15) +
       (h << 6) + (h >> 2);
  VN_MIX((int)in->extra_args_len);
  for (size_t k = 0; k < in->extra_args_len; ++k)
    VN_MIX(in->extra_args[k]);
  if (in->op == NYIR_CALL && in->imm > 0) {
    for (int64_t k = 0; k < in->imm; ++k)
      VN_MIX((int)(in->arg_sizes ? in->arg_sizes[k] : 0u));
  }
#undef VN_MIX
  return (size_t)(h % VN_TABLE_SIZE);
}

static bool vn_same_key(const vn_entry_t *e, const nyir_inst_t *in) {
  if (!e || !in || e->op != in->op || e->a != in->a || e->b != in->b ||
      e->c != in->c || e->d != in->d || e->e != in->e || e->f != in->f ||
      e->imm != in->imm || e->cmp != in->cmp || e->flags != in->flags ||
      !vn_symbol_equal(e->symbol, in->symbol) ||
      e->extra_args_len != in->extra_args_len)
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
static int vn_lookup(vn_entry_t *table, const nyir_inst_t *in) {
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
static void vn_insert(vn_entry_t *table, const nyir_inst_t *in,
                      int vn, int dst) {
  size_t idx = vn_hash(in);
  for (size_t probe = 0; probe < VN_TABLE_SIZE; ++probe) {
    vn_entry_t *e = &table[idx];
    if (!e->valid || vn_same_key(e, in)) {
      if (!e->valid) {
        e->op = in->op;
        e->a = in->a; e->b = in->b; e->c = in->c;
        e->d = in->d; e->e = in->e; e->f = in->f;
        e->imm = in->imm;
        e->cmp = in->cmp;
        e->flags = in->flags;
        e->symbol = in->symbol;
        e->extra_args = in->extra_args;
        e->extra_args_len = in->extra_args_len;
        e->arg_sizes = in->arg_sizes;
        e->dst = dst; /* keep the first (canonical) def for this vn */
      }
      e->vn = vn;
      e->valid = true;
      return;
    }
    idx = (idx + 1) % VN_TABLE_SIZE;
  }
}

static bool gvn_pass(nyir_func_t *f, int *vn_of_val, nyir_cfg_t *cfg) {
  if (!f || f->next_value <= 0)
    return true;

  vn_entry_t table[VN_TABLE_SIZE];
  vn_table_init(table);

  /*
   * value -> defining instruction index; vn -> canonical dst.  Both maps
   * make each redundant-hit check O(1) instead of a linear table/IR scan.
   */
  int *val_def = nyir_build_defs(f);
  if (!val_def)
    return true;
  int *vn_dst = malloc(((size_t)f->len + 1) * sizeof(*vn_dst));
  if (!vn_dst) {
    free(val_def);
    return true;
  }
  for (size_t j = 0; j < (size_t)f->len + 1; ++j)
    vn_dst[j] = -1;

  int next_vn = 0;
  for (size_t i = 0; i < f->len; ++i) {
    nyir_inst_t *in = &f->data[i];
    if (in->dst < 0 || in->dst >= (int)f->next_value)
      continue;
    if (in->op == NYIR_LOAD_I64 && gvn_load_pre(f, cfg, i))
      continue;


    if (!gvn_can_number(in))
      continue;

    int vn = vn_lookup(table, in);
    if (vn >= 0) {
      int cand = vn < (int)f->len + 1 ? vn_dst[vn] : -1;
      if (cand >= 0 && cand != in->dst && cand < f->next_value) {
        size_t def = (size_t)val_def[cand];
        if (def < f->len) {
          size_t def_block = cfg->inst_block[def];
          size_t use_block = cfg->inst_block[i];
          bool dom = (def_block == use_block && def < i) ||
                     (def_block != use_block &&
                      nyir_cfg_dominates(cfg, def_block, use_block));
          if (dom) {
            bool reads_memory =
                (nyir_effective_effects(in) & NYIR_EFFECT_READ_MEMORY) != 0;
            bool stable = !reads_memory ||
                          gvn_memory_stable_between(
                              f, cfg, def, i, in->op == NYIR_LOAD_I64);
            if (stable) {
              nyir_debug_loc_t debug = in->debug;
              nyir_range_t range = in->range;
              *in = (nyir_inst_t){.op = NYIR_COPY, .dst = in->dst,
                                   .a = cand, .b = -1, .c = -1, .d = -1,
                                   .e = -1, .f = -1, .imm = 0,
                                   .debug = debug, .range = range};
              continue;
            }
          }
        }
      }
      /*
       * Candidate didn't dominate: leave the computation (safe miss).
       */
      if (in->dst >= 0)
        vn_of_val[in->dst] = vn;
    } else {
      int new_vn = next_vn++;
      vn_insert(table, in, new_vn, in->dst);
      if (new_vn < (int)f->len + 1)
        vn_dst[new_vn] = in->dst;
      if (in->dst >= 0)
        vn_of_val[in->dst] = new_vn;
    }
  }
  free(vn_dst);
  free(val_def);
  return true;
}

bool nyir_gvn_pre(nyir_func_t *f) {
  if (!f || f->len < 2 || f->next_value <= 0)
    return true;

  nyir_cfg_t cfg = {0};
  if (!nyir_cfg_build(f, &cfg))
    return false;

  int *vn_of_val = calloc((size_t)f->next_value, sizeof(*vn_of_val));
  if (!vn_of_val) {
    nyir_cfg_free(&cfg);
    return false;
  }
  gvn_pass(f, vn_of_val, &cfg);
  free(vn_of_val);
  nyir_cfg_free(&cfg);
  return true;
}
