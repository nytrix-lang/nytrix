/*
 * Redundant-load elimination: removes memory loads when the same
 * location was recently loaded and no intervening store can alias it.
 *
 * References:
 *  Cooper & Torczon — Engineering a Compiler, 2nd Ed., Ch.10 §10.3.
 *  Muchnick — Advanced Compiler Design and Implementation, Ch.13 §13.3.
 */
#include "base/common.h"
#include "base/compat.h"
#include "code/backend/core/nyr.h"
#include "code/backend/passes/util.h"

static unsigned rle_mem_width(unsigned flags) {
  return (flags & NYIR_INST_F_MEM_BYTE)  ? 1u
         : (flags & NYIR_INST_F_MEM_U16) ? 2u
         : (flags & NYIR_INST_F_MEM_I32) ? 4u
                                         : 8u;
}

static bool rle_is_fresh_alloc(const nyr_inst_t *in) {
  if (!in)
    return false;
  if (in->op == NYIR_ALLOCA || in->op == NYIR_ADDR_SYMBOL)
    return true;
  if (in->op == NYIR_CALL && in->symbol &&
      (strcmp(in->symbol, "rt_tbuf_new_typed_raw") == 0 ||
       strcmp(in->symbol, "rt_tbuf_new_raw") == 0 ||
       strcmp(in->symbol, "malloc") == 0 ||
       strcmp(in->symbol, "calloc") == 0))
    return true;
  return false;
}

static bool rle_const_value(const nyr_func_t *f, const int *definition, int v,
                            int64_t *out, unsigned depth) {
  if (!f || !definition || v < 0 || v >= f->next_value || depth > 16)
    return false;
  int di = definition[v];
  if (di < 0 || (size_t)di >= f->len)
    return false;
  const nyr_inst_t *in = &f->data[di];
  if (in->op == NYIR_CONST_I64) {
    *out = in->imm;
    return true;
  }
  if (in->op == NYIR_COPY)
    return rle_const_value(f, definition, in->a, out, depth + 1);
  return false;
}

/*
 * Decompose an address expression into `(base_vreg, byte_offset)` across COPY
 * and constant ADD/SUB chains. When `is_root` is set, `base_vreg` is a proven
 * fresh allocation (`ALLOCA`, `rt_tbuf_new_*`, `malloc`/`calloc`, or global
 * `ADDR_SYMBOL`), enabling exact distinct-object and distinct-offset proofs.
 */
static void rle_addr_base_off(const nyr_func_t *f, const int *definition,
                              int addr, int *base_out, int64_t *off_out,
                              bool *is_root_out) {
  int cur = addr;
  int64_t off = 0;
  for (unsigned depth = 0; depth < 32 && cur >= 0 && cur < f->next_value;
       ++depth) {
    int di = definition[cur];
    if (di < 0 || (size_t)di >= f->len)
      break;
    const nyr_inst_t *in = &f->data[di];
    if (in->op == NYIR_COPY && in->a >= 0) {
      cur = in->a;
      continue;
    }
    if (in->op == NYIR_ADD_I64) {
      int64_t c = 0;
      if (rle_const_value(f, definition, in->b, &c, 0)) {
        off += c;
        cur = in->a;
        continue;
      }
      if (rle_const_value(f, definition, in->a, &c, 0)) {
        off += c;
        cur = in->b;
        continue;
      }
    } else if (in->op == NYIR_SUB_I64) {
      int64_t c = 0;
      if (rle_const_value(f, definition, in->b, &c, 0)) {
        off -= c;
        cur = in->a;
        continue;
      }
    }
    break;
  }
  *base_out = cur;
  *off_out = off;
  bool root = false;
  if (cur >= 0 && cur < f->next_value && definition[cur] >= 0)
    root = rle_is_fresh_alloc(&f->data[definition[cur]]);
  *is_root_out = root;
}

static bool rle_addrs_disjoint(const nyr_func_t *f, const int *definition,
                               int a1, unsigned flags1, int a2,
                               unsigned flags2) {
  if (!f || !definition || a1 < 0 || a2 < 0)
    return false;
  int b1 = -1, b2 = -1;
  int64_t o1 = 0, o2 = 0;
  bool r1 = false, r2 = false;
  rle_addr_base_off(f, definition, a1, &b1, &o1, &r1);
  rle_addr_base_off(f, definition, a2, &b2, &o2, &r2);
  if (b1 < 0 || b2 < 0)
    return false;
  if (b1 == b2) {
    int64_t w1 = (int64_t)rle_mem_width(flags1);
    int64_t w2 = (int64_t)rle_mem_width(flags2);
    return (o1 + w1 <= o2) || (o2 + w2 <= o1);
  }
  if (r1 && r2) {
    const nyr_inst_t *d1 = &f->data[definition[b1]];
    const nyr_inst_t *d2 = &f->data[definition[b2]];
    if (d1->op == NYIR_ADDR_SYMBOL && d2->op == NYIR_ADDR_SYMBOL)
      return d1->symbol && d2->symbol && strcmp(d1->symbol, d2->symbol) != 0;
    return true;
  }
  return false;
}

static bool rle_same_address(const nyr_func_t *f, const int *definition,
                             int lhs, int rhs, unsigned depth,
                             unsigned *budget) {
  if (lhs == rhs)
    return f && lhs >= 0 && lhs < f->next_value;
  if (!f || !definition || !budget || !*budget || lhs < 0 || rhs < 0 ||
      depth >= 32 ||
      lhs >= f->next_value || rhs >= f->next_value)
    return false;
  if (depth == 0) {
    int b1 = -1, b2 = -1;
    int64_t o1 = 0, o2 = 0;
    bool r1 = false, r2 = false;
    rle_addr_base_off(f, definition, lhs, &b1, &o1, &r1);
    rle_addr_base_off(f, definition, rhs, &b2, &o2, &r2);
    if (b1 >= 0 && b1 == b2 && o1 == o2)
      return true;
  }
  --*budget;
  int li = definition[lhs];
  int ri = definition[rhs];
  if (li < 0 || ri < 0)
    return false;
  const nyr_inst_t *l = &f->data[li];
  const nyr_inst_t *r = &f->data[ri];
  if (l->op != r->op)
    return false;
  if (l->op == NYIR_LOAD_LOCAL) {
    if (l->imm != r->imm || l->scalar_class != r->scalar_class)
      return false;
    size_t begin = li < ri ? (size_t)li : (size_t)ri;
    size_t end = li < ri ? (size_t)ri : (size_t)li;
    for (size_t i = begin + 1; i < end; ++i)
      if (f->data[i].op == NYIR_STORE_LOCAL && f->data[i].imm == l->imm)
        return false;
    return true;
  }
  if (l->op == NYIR_CONST_I64)
    return l->imm == r->imm;
  if (l->op == NYIR_ADDR_SYMBOL)
    return l->symbol && r->symbol && strcmp(l->symbol, r->symbol) == 0;
  if (l->op == NYIR_COPY)
    return rle_same_address(f, definition, l->a, r->a, depth + 1, budget);
  switch (l->op) {
  case NYIR_ADD_I64:
  case NYIR_MUL_I64:
    /*
     * Integer add/multiply are commutative, and frontends can build the
     * same indexed address with operands in either order.
     */
    return (rle_same_address(f, definition, l->a, r->a, depth + 1, budget) &&
            rle_same_address(f, definition, l->b, r->b, depth + 1, budget)) ||
           (rle_same_address(f, definition, l->a, r->b, depth + 1, budget) &&
            rle_same_address(f, definition, l->b, r->a, depth + 1, budget));
  case NYIR_SUB_I64:
  case NYIR_SHL_I64:
    return rle_same_address(f, definition, l->a, r->a, depth + 1, budget) &&
           rle_same_address(f, definition, l->b, r->b, depth + 1, budget);
  default:
    return false;
  }
}

/*
 * Redundant Load Elimination: after a STORE_LOCAL to slot S, if the
 * next LOAD_LOCAL from S has no intervening side effects, replace
 * with a COPY from the stored value.
 *
 * Carry the candidate store forward to every alias-safe load instead
 * of rescanning from every store. This gives a block-local mem2reg
 * win while keeping generated store-heavy code linear.
 */

#define NY_RLE_EDGE_CACHE 32

typedef struct {
  int address;
  int value;
  unsigned flags;
  nyr_scalar_class_t scalar_class;
} ny_rle_edge_fact_t;

typedef struct {
  size_t len;
  ny_rle_edge_fact_t facts[NY_RLE_EDGE_CACHE];
} ny_rle_edge_state_t;

static bool ny_rle_single_pred_edges(nyr_func_t *f) {
  nyr_cfg_t cfg = {0};
  if (!nyr_cfg_build_topology(f, &cfg))
    return false;
  if (!cfg.block_count) {
    nyr_cfg_free(&cfg);
    return true;
  }
  ny_rle_edge_state_t *out = calloc(cfg.block_count, sizeof(*out));
  if (!out) {
    nyr_cfg_free(&cfg);
    return false;
  }
  for (size_t block = 0; block < cfg.block_count; ++block) {
    if (!cfg.reachable[block])
      continue;
    ny_rle_edge_state_t state = {0};
    size_t p0 = cfg.pred_offsets[block];
    if (block && cfg.pred_offsets[block + 1] == p0 + 1) {
      size_t pred = cfg.pred_blocks[p0];
      if (pred < block && cfg.reachable[pred])
        state = out[pred];
    }
    for (size_t pc = cfg.block_start[block]; pc < cfg.block_end[block]; ++pc) {
      nyr_inst_t *in = &f->data[pc];
      if (in->op == NYIR_STORE_I64 || in->op == NYIR_STORE_LOCAL ||
          in->op == NYIR_COPY_STRUCT || in->op == NYIR_CALL ||
          in->op == NYIR_INLINE_ASM ||
          (nyr_effective_effects(in) &
           (NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_ALLOCATION |
            NYIR_EFFECT_UNKNOWN_SIDE_EFFECT | NYIR_EFFECT_IO |
            NYIR_EFFECT_FFI | NYIR_EFFECT_THREAD | NYIR_EFFECT_VOLATILE))) {
        state.len = 0;
        continue;
      }
      if (in->op != NYIR_LOAD_I64 || in->dst < 0 || in->a < 0 ||
          (in->effects & NYIR_EFFECT_VOLATILE))
        continue;
      unsigned flags = in->flags & (NYIR_INST_F_MEM_BYTE |
                                    NYIR_INST_F_MEM_U16 |
                                    NYIR_INST_F_MEM_I32);
      bool matched = false;
      for (size_t i = 0; i < state.len; ++i) {
        const ny_rle_edge_fact_t *fact = &state.facts[i];
        if (fact->address != in->a || fact->flags != flags ||
            fact->scalar_class != in->scalar_class)
          continue;
        nir_make_copy(in, fact->value);
        in->alias_class = 0;
        matched = true;
        break;
      }
      if (!matched) {
        if (state.len == NY_RLE_EDGE_CACHE) {
          memmove(state.facts, state.facts + 1,
                  (NY_RLE_EDGE_CACHE - 1) * sizeof(state.facts[0]));
          --state.len;
        }
        state.facts[state.len++] =
            (ny_rle_edge_fact_t){in->a, in->dst, flags, in->scalar_class};
      }
    }
    out[block] = state;
  }
  free(out);
  nyr_cfg_free(&cfg);
  return true;
}

static bool rle_small_memcpy_disabled(void) {
  static int disabled = -1;
  if (disabled < 0) {
    const char *env = getenv("NYTRIX_NO_NYIR_SMALL_MEMCPY");
    disabled = (env && *env && strcmp(env, "0") != 0) ? 1 : 0;
  }
  return disabled != 0;
}

static unsigned rle_width_flag(unsigned w) {
  if (w == 1u)
    return NYIR_INST_F_MEM_BYTE;
  if (w == 2u)
    return NYIR_INST_F_MEM_U16;
  if (w == 4u)
    return NYIR_INST_F_MEM_I32;
  return 0u;
}

static nyr_inst_t rle_make_inst(nyr_op_t op, int dst, int a, int b, int c,
                                int64_t imm, unsigned flags,
                                nyr_scalar_class_t sc, nyr_debug_loc_t debug) {
  nyr_inst_t in = {.op = op,
                   .dst = dst,
                   .a = a,
                   .b = b,
                   .c = c,
                   .d = -1,
                   .e = -1,
                   .f = -1,
                   .imm = imm,
                   .flags = flags,
                   .scalar_class = sc,
                   .debug = debug};
  in.effects = nyr_inst_effects(&in);
  return in;
}

static bool rle_is_direct_memcpy(const nyr_inst_t *in) {
  return in && in->op == NYIR_CALL && in->symbol &&
         strcmp(in->symbol, "memcpy") == 0 && in->imm == 3 &&
         !(in->flags & (NYIR_INST_F_INDIRECT | NYIR_INST_F_SRET |
                        NYIR_INST_F_SYMBOL_NONPREEMPTIBLE)) &&
         !(in->effects & NYIR_EFFECT_VOLATILE);
}

static bool rle_inst_uses_derived(const nyr_inst_t *in, int next_value,
                                  const bool *derived) {
  int ops[] = {in->a, in->b, in->c, in->d, in->e, in->f};
  for (size_t k = 0; k < sizeof(ops) / sizeof(ops[0]); ++k)
    if (ops[k] >= 0 && ops[k] < next_value && derived[ops[k]])
      return true;
  for (size_t k = 0; k < in->extra_args_len; ++k)
    if (in->extra_args[k] >= 0 && in->extra_args[k] < next_value &&
        derived[in->extra_args[k]])
      return true;
  for (size_t k = 0; k < in->phi_incoming_len; ++k)
    if (in->phi_incoming[k].value >= 0 &&
        in->phi_incoming[k].value < next_value &&
        derived[in->phi_incoming[k].value])
      return true;
  return false;
}

/*
 * Recover known little-endian byte values for [src .. src + size) when `src`
 * points into a non-escaping NYIR_ALLOCA populated solely by dominating
 * constant stores in one basic block.
 */
static bool rle_const_alloca_bytes(const nyr_func_t *f, const nyr_cfg_t *cfg,
                                   const int *defs, size_t pc, int src,
                                   int64_t size, uint8_t bytes[8]) {
  if (!f || !cfg || !defs || size < 1 || size > 8 || pc >= f->len)
    return false;
  int src_base = -1;
  int64_t src_off = 0;
  bool src_root = false;
  rle_addr_base_off(f, defs, src, &src_base, &src_off, &src_root);
  if (src_base < 0 || src_base >= f->next_value || defs[src_base] < 0)
    return false;
  const nyr_inst_t *base_inst = &f->data[defs[src_base]];
  if (base_inst->op != NYIR_ALLOCA || src_off < 0 || base_inst->imm < size ||
      src_off + size > base_inst->imm)
    return false;

  bool *derived = calloc((size_t)f->next_value, sizeof(*derived));
  if (!derived)
    return false;
  derived[src_base] = true;
  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t k = 0; k < f->len; ++k) {
      const nyr_inst_t *in = &f->data[k];
      if (in->op == NYIR_NOP || in->dst < 0 || in->dst >= f->next_value ||
          derived[in->dst])
        continue;
      int64_t c = 0;
      if (in->op == NYIR_COPY && in->a >= 0 && in->a < f->next_value &&
          derived[in->a]) {
        derived[in->dst] = true;
        changed = true;
      } else if (in->op == NYIR_ADD_I64 && in->a >= 0 && in->b >= 0 &&
                 in->a < f->next_value && in->b < f->next_value &&
                 ((derived[in->a] && !derived[in->b] &&
                   rle_const_value(f, defs, in->b, &c, 0)) ||
                  (derived[in->b] && !derived[in->a] &&
                   rle_const_value(f, defs, in->a, &c, 0)))) {
        derived[in->dst] = true;
        changed = true;
      } else if (in->op == NYIR_SUB_I64 && in->a >= 0 && in->b >= 0 &&
                 in->a < f->next_value && in->b < f->next_value &&
                 derived[in->a] && !derived[in->b] &&
                 rle_const_value(f, defs, in->b, &c, 0)) {
        derived[in->dst] = true;
        changed = true;
      }
    }
  }

  size_t pc_blk = cfg->inst_block[pc];
  size_t store_blk = SIZE_MAX;
  bool safe = true;
  for (size_t k = 0; k < f->len && safe; ++k) {
    const nyr_inst_t *in = &f->data[k];
    if (in->op == NYIR_NOP || !rle_inst_uses_derived(in, f->next_value, derived))
      continue;
    if (in->op == NYIR_COPY || in->op == NYIR_ADD_I64 ||
        in->op == NYIR_SUB_I64) {
      if (in->dst < 0 || in->dst >= f->next_value || !derived[in->dst] ||
          in->extra_args_len != 0 || in->phi_incoming_len != 0 ||
          in->c >= 0 || in->d >= 0 || in->e >= 0 || in->f >= 0)
        safe = false;
      continue;
    }
    if (in->op == NYIR_LOAD_I64) {
      if (in->a < 0 || in->a >= f->next_value || !derived[in->a] ||
          in->b >= 0 || in->c >= 0 || in->d >= 0 || in->e >= 0 ||
          in->f >= 0 || in->extra_args_len != 0 || in->phi_incoming_len != 0)
        safe = false;
      continue;
    }
    if (in->op == NYIR_COPY_STRUCT) {
      if (in->b < 0 || in->b >= f->next_value || !derived[in->b] ||
          (in->a >= 0 && in->a < f->next_value && derived[in->a]) ||
          in->c >= 0 || in->d >= 0 || in->e >= 0 || in->f >= 0 ||
          in->extra_args_len != 0 || in->phi_incoming_len != 0)
        safe = false;
      continue;
    }
    if (rle_is_direct_memcpy(in)) {
      int args[3] = {0};
      int argc = 0;
      if (in->phi_incoming_len != 0 ||
          !nyr_call_args(in, f->next_value, args, 3, &argc, NULL, 0) ||
          argc != 3 || args[0] < 0 || args[2] < 0 ||
          (args[0] < f->next_value && derived[args[0]]) ||
          (args[2] < f->next_value && derived[args[2]]))
        safe = false;
      continue;
    }
    if (in->op == NYIR_STORE_I64 && !(in->effects & NYIR_EFFECT_VOLATILE) &&
        in->a >= 0 && in->a < f->next_value && derived[in->a] &&
        !(in->c >= 0 && in->c < f->next_value && derived[in->c]) &&
        in->b < 0 && in->d < 0 && in->e < 0 && in->f < 0 &&
        in->extra_args_len == 0 && in->phi_incoming_len == 0) {
      int st_base = -1;
      int64_t st_off = 0;
      int64_t st_val = 0;
      bool st_root = false;
      rle_addr_base_off(f, defs, in->a, &st_base, &st_off, &st_root);
      unsigned w = rle_mem_width(in->flags);
      if (st_base != src_base || st_off < 0 ||
          st_off + (int64_t)w > base_inst->imm ||
          !rle_const_value(f, defs, in->c, &st_val, 0)) {
        safe = false;
        break;
      }
      size_t sblk = cfg->inst_block[k];
      if (sblk == pc_blk) {
        if (k >= pc) {
          safe = false;
          break;
        }
      } else if (!nyr_cfg_dominates(cfg, sblk, pc_blk)) {
        safe = false;
        break;
      }
      if (store_blk == SIZE_MAX)
        store_blk = sblk;
      else if (store_blk != sblk) {
        safe = false;
        break;
      }
      continue;
    }
    safe = false;
  }

  if (!safe || store_blk == SIZE_MAX) {
    free(derived);
    return false;
  }

  bool valid[8] = {false};
  memset(bytes, 0, 8);
  size_t end = (store_blk == pc_blk) ? pc : cfg->block_end[store_blk];
  for (size_t k = cfg->block_start[store_blk]; k < end; ++k) {
    const nyr_inst_t *in = &f->data[k];
    if (in->op != NYIR_STORE_I64 || in->a < 0 || in->a >= f->next_value ||
        !derived[in->a])
      continue;
    int st_base = -1;
    int64_t st_off = 0;
    int64_t st_val = 0;
    bool st_root = false;
    rle_addr_base_off(f, defs, in->a, &st_base, &st_off, &st_root);
    if (!rle_const_value(f, defs, in->c, &st_val, 0)) {
      free(derived);
      return false;
    }
    unsigned w = rle_mem_width(in->flags);
    uint64_t uv = (uint64_t)st_val;
    for (unsigned b = 0; b < w; ++b) {
      int64_t pos = st_off + (int64_t)b;
      if (pos >= src_off && pos < src_off + size) {
        size_t rel = (size_t)(pos - src_off);
        bytes[rel] = (uint8_t)((uv >> (b * 8u)) & 0xffu);
        valid[rel] = true;
      }
    }
  }
  free(derived);
  for (int64_t i = 0; i < size; ++i)
    if (!valid[i])
      return false;
  return true;
}

typedef struct {
  size_t pc;
  int dst;
  int src;
  int64_t size;
  int ret_dst;
  nyr_scalar_class_t ret_sc;
  nyr_debug_loc_t debug;
  nyr_inst_t rep[24];
  size_t rep_len;
} rle_memcpy_plan_t;

static int rle_emit_ptr_offset(nyr_inst_t *rep, size_t *n, nyr_func_t *cand,
                               int base, int64_t off, nyr_debug_loc_t debug) {
  if (off == 0)
    return base;
  int off_v = cand->next_value++;
  rep[(*n)++] = rle_make_inst(NYIR_CONST_I64, off_v, -1, -1, -1, off, 0,
                              NYIR_SCALAR_I64_RAW, debug);
  int addr_v = cand->next_value++;
  rep[(*n)++] = rle_make_inst(NYIR_ADD_I64, addr_v, base, off_v, -1, 0, 0,
                              NYIR_SCALAR_I64_RAW, debug);
  return addr_v;
}

static bool rle_expand_small_memcpy(nyr_func_t *f) {
  bool any_cand = false;
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if ((in->op == NYIR_COPY_STRUCT && !(in->effects & NYIR_EFFECT_VOLATILE) &&
         in->a >= 0 && in->b >= 0 && in->c < 0 && in->dst < 0 &&
         in->imm >= 1 && in->imm <= 8) ||
        rle_is_direct_memcpy(in)) {
      any_cand = true;
      break;
    }
  }
  if (!any_cand)
    return true;

  nyr_func_t cand = {0};
  if (!nyr_ir_func_clone(f, &cand))
    return false;
  int *defs = nyr_build_defs(&cand);
  nyr_cfg_t cfg = {0};
  if (!defs || !nyr_cfg_build(&cand, &cfg)) {
    free(defs);
    nyr_cfg_free(&cfg);
    nyr_ir_func_free(&cand);
    return false;
  }

  rle_memcpy_plan_t plans[64];
  size_t plan_count = 0;
  for (size_t i = 0; i < cand.len && plan_count < 64; ++i) {
    const nyr_inst_t *in = &cand.data[i];
    if (in->op == NYIR_COPY_STRUCT && !(in->effects & NYIR_EFFECT_VOLATILE) &&
        in->a >= 0 && in->b >= 0 && in->c < 0 && in->dst < 0 &&
        in->imm >= 1 && in->imm <= 8 && in->a < cand.next_value &&
        in->b < cand.next_value) {
      plans[plan_count++] = (rle_memcpy_plan_t){.pc = i,
                                                .dst = in->a,
                                                .src = in->b,
                                                .size = in->imm,
                                                .ret_dst = -1,
                                                .ret_sc = NYIR_SCALAR_UNKNOWN,
                                                .debug = in->debug};
    } else if (rle_is_direct_memcpy(in)) {
      int args[3] = {0};
      int argc = 0;
      int64_t sz = 0;
      if (nyr_call_args(in, cand.next_value, args, 3, &argc, NULL, 0) &&
          argc == 3 && args[0] >= 0 && args[0] < cand.next_value &&
          args[1] >= 0 && args[1] < cand.next_value &&
          rle_const_value(&cand, defs, args[2], &sz, 0) && sz >= 1 &&
          sz <= 8) {
        plans[plan_count++] = (rle_memcpy_plan_t){
            .pc = i,
            .dst = args[0],
            .src = args[1],
            .size = sz,
            .ret_dst = in->dst,
            .ret_sc = in->scalar_class,
            .debug = in->debug,
        };
      }
    }
  }

  if (plan_count == 0) {
    free(defs);
    nyr_cfg_free(&cfg);
    nyr_ir_func_free(&cand);
    return true;
  }

  size_t total_extra = 0;
  for (size_t p = 0; p < plan_count; ++p) {
    rle_memcpy_plan_t *pl = &plans[p];
    int64_t chunk_off[4] = {0};
    unsigned chunk_w[4] = {0};
    unsigned nchunks = 0;
    for (int64_t off = 0, rem = pl->size; rem > 0;) {
      unsigned w = rem >= 8 ? 8u : rem >= 4 ? 4u : rem >= 2 ? 2u : 1u;
      chunk_off[nchunks] = off;
      chunk_w[nchunks] = w;
      ++nchunks;
      off += (int64_t)w;
      rem -= (int64_t)w;
    }

    uint8_t cbytes[8] = {0};
    size_t n = 0;
    if (rle_const_alloca_bytes(&cand, &cfg, defs, pl->pc, pl->src, pl->size,
                               cbytes)) {
      for (unsigned ci = 0; ci < nchunks; ++ci) {
        int64_t off = chunk_off[ci];
        unsigned w = chunk_w[ci];
        uint64_t packed = 0;
        for (unsigned b = 0; b < w; ++b)
          packed |= ((uint64_t)cbytes[off + b]) << (b * 8u);
        int addr_v =
            rle_emit_ptr_offset(pl->rep, &n, &cand, pl->dst, off, pl->debug);
        int val_v = cand.next_value++;
        pl->rep[n++] =
            rle_make_inst(NYIR_CONST_I64, val_v, -1, -1, -1, (int64_t)packed, 0,
                          NYIR_SCALAR_I64_RAW, pl->debug);
        pl->rep[n++] =
            rle_make_inst(NYIR_STORE_I64, -1, addr_v, -1, val_v, 0,
                          rle_width_flag(w), NYIR_SCALAR_I64_RAW, pl->debug);
      }
    } else {
      int tmps[4] = {-1, -1, -1, -1};
      for (unsigned ci = 0; ci < nchunks; ++ci) {
        int src_v = rle_emit_ptr_offset(pl->rep, &n, &cand, pl->src,
                                        chunk_off[ci], pl->debug);
        tmps[ci] = cand.next_value++;
        pl->rep[n++] = rle_make_inst(NYIR_LOAD_I64, tmps[ci], src_v, -1, -1, 0,
                                     rle_width_flag(chunk_w[ci]),
                                     NYIR_SCALAR_I64_RAW, pl->debug);
      }
      for (unsigned ci = 0; ci < nchunks; ++ci) {
        int dst_v = rle_emit_ptr_offset(pl->rep, &n, &cand, pl->dst,
                                        chunk_off[ci], pl->debug);
        pl->rep[n++] = rle_make_inst(NYIR_STORE_I64, -1, dst_v, -1, tmps[ci], 0,
                                     rle_width_flag(chunk_w[ci]),
                                     NYIR_SCALAR_UNKNOWN, pl->debug);
      }
    }
    if (pl->ret_dst >= 0) {
      pl->rep[n++] = rle_make_inst(NYIR_COPY, pl->ret_dst, pl->dst, -1, -1, 0,
                                   0, pl->ret_sc, pl->debug);
    }
    pl->rep_len = n;
    if (n > 1)
      total_extra += n - 1;
  }

  free(defs);
  nyr_cfg_free(&cfg);

  if (total_extra > 0 && !nir_ensure_inst_space(&cand, total_extra)) {
    nyr_ir_func_free(&cand);
    return false;
  }

  for (size_t p = plan_count; p > 0; --p) {
    const rle_memcpy_plan_t *pl = &plans[p - 1];
    size_t pc = pl->pc;
    nyr_inst_discard(&cand.data[pc]);
    if (pl->rep_len > 1) {
      memmove(&cand.data[pc + pl->rep_len], &cand.data[pc + 1],
              (cand.len - (pc + 1)) * sizeof(*cand.data));
      cand.len += pl->rep_len - 1;
    }
    for (size_t k = 0; k < pl->rep_len; ++k)
      cand.data[pc + k] = pl->rep[k];
  }

  char err[256] = {0};
  if (!nyr_ir_verify(&cand, err, sizeof(err))) {
    nyr_ir_func_free(&cand);
    return true;
  }
  nyr_func_replace(f, &cand);
  return true;
}

bool nyr_redundant_load_elim(nyr_func_t *f) {
  if (!f || f->next_value <= 0)
    return true;
  if (!rle_small_memcpy_disabled() && !rle_expand_small_memcpy(f))
    return false;
  size_t count = nyr_max_local(f);
  bool has_memory_address = false;
  for (size_t i = 0; i < f->len; ++i) {
    nyr_op_t op = f->data[i].op;
    if (op == NYIR_LOAD_I64 || op == NYIR_STORE_I64 ||
        op == NYIR_VEC4_LOAD_F64 || op == NYIR_VEC8_LOAD_F32 ||
        op == NYIR_VEC4_LOAD_I64 || op == NYIR_VEC8_LOAD_I64 ||
        op == NYIR_VEC4_STORE_F64 || op == NYIR_VEC8_STORE_F32 ||
        op == NYIR_VEC4_STORE_I64 || op == NYIR_VEC8_STORE_I64) {
      has_memory_address = true;
      break;
    }
  }

  int *available_value =
      count ? (int *)malloc(count * sizeof(*available_value)) : NULL;
  unsigned *available_epoch =
      count ? (unsigned *)calloc(count, sizeof(*available_epoch)) : NULL;
  bool *escaped = count ? (bool *)calloc(count, sizeof(*escaped)) : NULL;
  uint32_t *value_alias =
      (uint32_t *)calloc((size_t)f->next_value, sizeof(*value_alias));
  int *definition =
      has_memory_address
          ? (int *)malloc((size_t)f->next_value * sizeof(*definition))
          : NULL;
  if ((count && (!available_value || !available_epoch || !escaped)) ||
      !value_alias || (has_memory_address && !definition)) {
    free(available_value);
    free(available_epoch);
    free(escaped);
    free(value_alias);
    free(definition);
    return false;
  }
  if (definition)
    for (int v = 0; v < f->next_value; ++v)
      definition[v] = -1;
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->dst < 0 || in->dst >= f->next_value)
      continue;
    if (definition)
      definition[in->dst] = (int)i;
    switch (in->op) {
    case NYIR_ADDR_LOCAL:
    case NYIR_ADDR_SYMBOL:
    case NYIR_ALLOCA:
    case NYIR_ADD_I64:
    case NYIR_SUB_I64:
      value_alias[in->dst] = in->alias_class;
      break;
    case NYIR_COPY:
      /*
       * COPY carries value provenance. A load's alias_class describes the
       * address it reads, not a pointer value stored at that address.
       */
      if (in->a >= 0 && in->a < f->next_value)
        value_alias[in->dst] = value_alias[in->a];
      break;
    default:
      /*
       * Loaded pointers, call results, PHIs, and unknown producers remain
       * may-alias-anything until a dedicated value-provenance fact exists.
       */
      break;
    }
  }
  /*
   * A local whose address is never materialized cannot be observed or
   * clobbered by a call.  Compute this once for the whole function instead
   * of treating every call as a blanket local-memory barrier.  Conversely,
   * never forward an address-taken local here: generic pointer loads/stores
   * may alias it even without a call.
   */
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->op == NYIR_ADDR_LOCAL && in->imm >= 0 && (size_t)in->imm < count)
      escaped[in->imm] = true;
  }
#define NY_MEM_FORWARD_MAX 64
  typedef struct {
    int addr;
    int value;
    unsigned flags;
    nyr_scalar_class_t scalar_class;
  } ny_mem_avail_t;
  ny_mem_avail_t mem_avail[NY_MEM_FORWARD_MAX];
  size_t mem_avail_count = 0;
  int vector_addr = -1;
  int vector_value = -1;
  nyr_op_t vector_store_op = NYIR_NOP;

  unsigned epoch = 1;
  for (size_t i = 0; i < f->len; ++i) {
    nyr_inst_t *in = &f->data[i];
    if (in->op == NYIR_LABEL || in->op == NYIR_BR || in->op == NYIR_BR_IF ||
        in->op == NYIR_RET) {
      mem_avail_count = 0;
      vector_addr = vector_value = -1;
      vector_store_op = NYIR_NOP;
      if (++epoch == 0) {
        if (count)
          memset(available_epoch, 0, count * sizeof(*available_epoch));
        epoch = 1;
      }
      continue;
    }
    if (in->op == NYIR_CALL) {
      unsigned eff = nyr_effective_effects(in);
      if (eff & (NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_ALLOCATION |
                 NYIR_EFFECT_UNKNOWN_SIDE_EFFECT | NYIR_EFFECT_IO |
                 NYIR_EFFECT_FFI)) {
        mem_avail_count = 0;
        vector_addr = vector_value = -1;
        vector_store_op = NYIR_NOP;
      }
      /*
       * A call can mutate a local through an escaped address or captured
       * environment.  The escape summary is function-local and cannot see
       * aliases held by the callee, so do not carry STORE_LOCAL forwarding
       * facts across call boundaries.
       */
      if (count)
        memset(available_epoch, 0, count * sizeof(*available_epoch));
      continue;
    }
    if (in->op == NYIR_STORE_I64) {
      vector_addr = vector_value = -1;
      vector_store_op = NYIR_NOP;
      int addr = in->a;
      int val = in->c;
      unsigned flags = in->flags & (NYIR_INST_F_MEM_BYTE | NYIR_INST_F_MEM_U16 |
                                    NYIR_INST_F_MEM_I32);
      /*
       * A store through one SSA address kills every other cached
       * address entry: distinct SSA values can denote the same memory
       * and this block-local pass has no alias proof. Only the stored
       * address's own entry survives, updated to the new value.
       */
      size_t w = 0;
      uint32_t store_alias =
          addr >= 0 && addr < f->next_value ? value_alias[addr] : 0;
      bool stored_entry = false;
      for (size_t m = 0; m < mem_avail_count; ++m) {
        if (addr >= 0 && val >= 0 &&
            (mem_avail[m].addr == addr ||
             rle_same_address(f, definition, addr, mem_avail[m].addr, 0,
                              &(unsigned){128})) &&
            mem_avail[m].flags == flags &&
            mem_avail[m].scalar_class == in->scalar_class) {
          mem_avail[w].addr = addr;
          mem_avail[w].value = val;
          mem_avail[w].flags = flags;
          mem_avail[w].scalar_class = in->scalar_class;
          ++w;
          stored_entry = true;
          continue;
        }
        uint32_t cached_alias =
            mem_avail[m].addr >= 0 && mem_avail[m].addr < f->next_value
                ? value_alias[mem_avail[m].addr]
                : 0;
        if ((store_alias && cached_alias && store_alias != cached_alias) ||
            rle_addrs_disjoint(f, definition, addr, flags, mem_avail[m].addr,
                               mem_avail[m].flags))
          mem_avail[w++] = mem_avail[m];
      }
      mem_avail_count = w;
      if (!stored_entry && addr >= 0 && val >= 0 &&
          mem_avail_count < NY_MEM_FORWARD_MAX)
        mem_avail[mem_avail_count++] =
            (ny_mem_avail_t){addr, val, flags, in->scalar_class};
      continue;
    }
    /*
     * Raw vector writes may hit any tracked address. Aggregate copy has two
     * independent memory operands: .a is its destination and .b is read-only
     * source. A known destination class invalidates only cached addresses in
     * that class; unknown destination or cached classes remain barriers.
     */
    if (in->op == NYIR_COPY_STRUCT) {
      vector_addr = vector_value = -1;
      vector_store_op = NYIR_NOP;
      uint32_t dest_alias =
          in->a >= 0 && in->a < f->next_value ? value_alias[in->a] : 0;
      if (!dest_alias) {
        mem_avail_count = 0;
      } else {
        size_t w = 0;
        for (size_t m = 0; m < mem_avail_count; ++m) {
          uint32_t cached_alias =
              mem_avail[m].addr >= 0 && mem_avail[m].addr < f->next_value
                  ? value_alias[mem_avail[m].addr]
                  : 0;
          if (!cached_alias || cached_alias == dest_alias)
            continue;
          mem_avail[w++] = mem_avail[m];
        }
        mem_avail_count = w;
      }
      continue;
    }
    if (in->op == NYIR_VEC4_STORE_F64 || in->op == NYIR_VEC8_STORE_F32 ||
        in->op == NYIR_VEC4_STORE_I64 || in->op == NYIR_VEC8_STORE_I64) {
      mem_avail_count = 0;
      vector_addr = in->a;
      vector_value = in->b >= 0 ? in->b : in->c;
      vector_store_op = in->op;
      continue;
    }
    if (in->op == NYIR_VEC4_LOAD_F64 || in->op == NYIR_VEC8_LOAD_F32 ||
        in->op == NYIR_VEC4_LOAD_I64 || in->op == NYIR_VEC8_LOAD_I64) {
      bool matching_type = (in->op == NYIR_VEC4_LOAD_F64 &&
                            vector_store_op == NYIR_VEC4_STORE_F64) ||
                           (in->op == NYIR_VEC8_LOAD_F32 &&
                            vector_store_op == NYIR_VEC8_STORE_F32) ||
                           (in->op == NYIR_VEC4_LOAD_I64 &&
                            vector_store_op == NYIR_VEC4_STORE_I64) ||
                           (in->op == NYIR_VEC8_LOAD_I64 &&
                            vector_store_op == NYIR_VEC8_STORE_I64);
      if (matching_type && vector_value >= 0 && in->dst >= 0 && definition &&
          rle_same_address(f, definition, in->a, vector_addr, 0,
                           &(unsigned){128})) {
        nir_make_copy(in, vector_value);
        in->alias_class =
            vector_value < f->next_value ? value_alias[vector_value] : 0;
      }
      continue;
    }
    if (vector_value >= 0 &&
        (nyr_effective_effects(in) &
         (NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_ALLOCATION |
          NYIR_EFFECT_UNKNOWN_SIDE_EFFECT | NYIR_EFFECT_IO |
          NYIR_EFFECT_FFI | NYIR_EFFECT_THREAD | NYIR_EFFECT_VOLATILE))) {
      vector_addr = vector_value = -1;
      vector_store_op = NYIR_NOP;
    }
    if (in->op == NYIR_LOAD_I64 && in->dst >= 0 &&
        !(in->effects & NYIR_EFFECT_VOLATILE)) {
      int addr = in->a;
      unsigned flags = in->flags & (NYIR_INST_F_MEM_BYTE | NYIR_INST_F_MEM_U16 |
                                    NYIR_INST_F_MEM_I32);
      if (addr >= 0) {
        bool forwarded = false;
        for (size_t m = 0; m < mem_avail_count; ++m) {
          if (mem_avail[m].flags == flags &&
              rle_same_address(f, definition, addr, mem_avail[m].addr, 0,
                               &(unsigned){128}) &&
              (mem_avail[m].scalar_class == in->scalar_class ||
               in->scalar_class == NYIR_SCALAR_I64_RAW)) {
            /*
             * Replace the load through the shared shape constructor so
             * metadata derived from the old memory read (notably range and
             * alias facts) cannot survive as facts about the copied value.
             */
            nir_make_copy(in, mem_avail[m].value);
            in->alias_class =
                mem_avail[m].value >= 0 && mem_avail[m].value < f->next_value
                    ? value_alias[mem_avail[m].value]
                    : 0;
            forwarded = true;
            break;
          }
        }
        if (!forwarded) {
          bool found = false;
          for (size_t m = 0; m < mem_avail_count; ++m) {
            if (mem_avail[m].addr == addr) {
              mem_avail[m].value = in->dst;
              mem_avail[m].flags = flags;
              found = true;
              break;
            }
          }
          if (!found && mem_avail_count < NY_MEM_FORWARD_MAX) {
            mem_avail[mem_avail_count++] =
                (ny_mem_avail_t){addr, in->dst, flags, in->scalar_class};
          }
        }
      }
      continue;
    }
    if (in->op == NYIR_INLINE_ASM || in->op == NYIR_STORE_LOCAL ||
        (nyr_effective_effects(in) &
         (NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_ALLOCATION |
          NYIR_EFFECT_UNKNOWN_SIDE_EFFECT | NYIR_EFFECT_IO |
          NYIR_EFFECT_FFI | NYIR_EFFECT_THREAD | NYIR_EFFECT_VOLATILE))) {
      mem_avail_count = 0;
      vector_addr = vector_value = -1;
      vector_store_op = NYIR_NOP;
    }
    if (in->imm < 0 || (size_t)in->imm >= count)
      continue;
    size_t slot = (size_t)in->imm;

    if (in->op == NYIR_ADDR_LOCAL || escaped[slot]) {
      available_epoch[slot] = 0;
      continue;
    }
    if (in->op == NYIR_STORE_LOCAL) {
      available_value[slot] = in->a;
      available_epoch[slot] = epoch;
      continue;
    }
    if (in->op == NYIR_LOAD_LOCAL && in->dst >= 0) {
      if (available_epoch[slot] == epoch) {
        int source = available_value[slot];
        uint32_t source_alias =
            source >= 0 && source < f->next_value ? value_alias[source] : 0;
        nir_make_copy(in, source);
        /*
         * The source fact was computed from value provenance above; the old
         * LOAD_LOCAL alias class described the slot access, not this value.
         */
        in->alias_class = source_alias;
      } else {
        available_value[slot] = in->dst;
        available_epoch[slot] = epoch;
      }
    }
  }
#undef NY_MEM_FORWARD_MAX
  free(available_value);
  free(available_epoch);
  free(escaped);
  free(value_alias);
  free(definition);
  return ny_rle_single_pred_edges(f);
}
