/*
 * Register promotion of global scalar slots inside call-free loops.
 *
 * A top-level `mut` lives in a linker-resolved slot, so every loop iteration
 * reloads and re-stores it and range facts about it (an induction counter,
 * say) are lost. When a natural loop contains no calls and no memory access
 * other than plain 64-bit loads/stores of directly addressed symbols, no other
 * code can observe those slots while the loop runs. Each such slot is then
 * given a fresh local: it is loaded once in the preheader, accessed as a local
 * inside the loop, and written back on entry to every exit block. mem2reg
 * turns the local into SSA, exposing the loop-carried PHI to VRP and SCEV.
 *
 * Legality, all conservative:
 *   - no instruction in the loop carries CALL, VOLATILE, THREAD, FFI, IO,
 *     ALLOCATION or UNKNOWN_SIDE_EFFECT;
 *   - every memory access in the loop is a full-width LOAD_I64/STORE_I64
 *     whose address is an NYIR_ADDR_SYMBOL;
 *   - the header has exactly one outside predecessor, whose only successor
 *     is the header (a dedicated preheader);
 *   - every exit block has only loop predecessors, so the write-back runs
 *     exactly when control leaves the loop.
 */
#include "code/backend/ir/opt/util.h"
#include <stdlib.h>
#include <string.h>

enum { LGP_MAX_ROUNDS = 8, LGP_MAX_SLOTS = 16, LGP_MAX_EXITS = 8 };

typedef struct {
  const char *symbol;
  int local;
  /*
   * Original instructions whose metadata (scalar class, memory flags, alias
   * class, debug location) the inserted seed and write-back copy.
   */
  nyr_inst_t addr_tmpl, load_tmpl, store_tmpl;
  bool has_load, has_store;
} lgp_slot_t;

/*
 * Copy metadata only; the clone owns no out-of-line arrays.
 */
static nyr_inst_t lgp_clone(const nyr_inst_t *tmpl, nyr_op_t op, int dst, int a,
                            int c, int64_t imm) {
  nyr_inst_t in = *tmpl;
  in.op = op;
  in.dst = dst;
  in.a = a;
  in.b = in.c = in.d = in.e = in.f = -1;
  in.c = c;
  in.imm = imm;
  in.extra_args = NULL;
  in.extra_args_len = 0;
  in.arg_sizes = NULL;
  in.phi_incoming = NULL;
  in.phi_incoming_len = 0;
  return in;
}

typedef struct {
  size_t at;
  nyr_inst_t inst;
} lgp_insert_t;

static const char *lgp_symbol_of(const nyr_func_t *f, const int *defs,
                                 int value) {
  if (value < 0 || value >= f->next_value || defs[value] < 0)
    return NULL;
  const nyr_inst_t *def = &f->data[defs[value]];
  return def->op == NYIR_ADDR_SYMBOL && def->symbol && *def->symbol
             ? def->symbol
             : NULL;
}

static bool lgp_plain_width(const nyr_inst_t *in) {
  return !(in->flags & (NYIR_INST_F_MEM_BYTE | NYIR_INST_F_MEM_I32 |
                        NYIR_INST_F_MEM_U16 | NYIR_INST_F_MEM_VOLATILE));
}

static size_t lgp_next_local(const nyr_func_t *f) {
  size_t n = f->param_count;
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if ((in->op == NYIR_LOAD_LOCAL || in->op == NYIR_STORE_LOCAL ||
         in->op == NYIR_ADDR_LOCAL) &&
        in->imm >= 0 && (size_t)in->imm + 1 > n)
      n = (size_t)in->imm + 1;
  }
  return n;
}

/*
 * Index at which code ending a block may be inserted: before its terminator
 * when it has one, otherwise at the block end (fallthrough).
 */
static size_t lgp_block_tail(const nyr_func_t *f, const nyr_cfg_t *cfg,
                             size_t block) {
  size_t end = nyr_block_last_non_nop(f, cfg, block);
  if (end > cfg->block_start[block]) {
    nyr_op_t op = f->data[end - 1].op;
    if (op == NYIR_BR || op == NYIR_BR_IF || op == NYIR_RET)
      return end - 1;
  }
  return end;
}

/*
 * First index after the block's label and PHIs.
 */
static size_t lgp_block_head(const nyr_func_t *f, const nyr_cfg_t *cfg,
                             size_t block) {
  size_t at = cfg->block_start[block];
  size_t end = cfg->block_end[block];
  while (at < end && (f->data[at].op == NYIR_LABEL ||
                      f->data[at].op == NYIR_PHI || f->data[at].op == NYIR_NOP))
    ++at;
  return at;
}

static const unsigned lgp_barrier_effects =
    NYIR_EFFECT_CALL | NYIR_EFFECT_VOLATILE | NYIR_EFFECT_THREAD |
    NYIR_EFFECT_FFI | NYIR_EFFECT_IO | NYIR_EFFECT_ALLOCATION |
    NYIR_EFFECT_UNKNOWN_SIDE_EFFECT;

/*
 * Try one natural loop. Returns 1 when it was rewritten, 0 when it is not a
 * candidate, -1 on allocation failure.
 */
static int lgp_try_loop(nyr_func_t *f, const nyr_cfg_t *cfg, const int *defs,
                        size_t header, const bool *member) {
  size_t nb = cfg->block_count;
  lgp_slot_t slots[LGP_MAX_SLOTS];
  size_t slot_len = 0;
  for (size_t b = 0; b < nb; ++b) {
    if (!member[b])
      continue;
    for (size_t i = cfg->block_start[b]; i < cfg->block_end[b]; ++i) {
      const nyr_inst_t *in = &f->data[i];
      if (in->op == NYIR_NOP)
        continue;
      if (in->op == NYIR_CALL || (nyr_inst_effects(in) & lgp_barrier_effects))
        return 0;
      if (in->op == NYIR_LOAD_I64 || in->op == NYIR_STORE_I64) {
        const char *sym = lgp_symbol_of(f, defs, in->a);
        if (!sym || !lgp_plain_width(in))
          return 0;
        size_t k = 0;
        while (k < slot_len && strcmp(slots[k].symbol, sym) != 0)
          ++k;
        if (k == slot_len) {
          if (slot_len == LGP_MAX_SLOTS)
            return 0;
          slots[slot_len++] = (lgp_slot_t){
              .symbol = sym, .local = -1, .addr_tmpl = f->data[defs[in->a]]};
        }
        if (in->op == NYIR_LOAD_I64 && !slots[k].has_load) {
          slots[k].load_tmpl = *in;
          slots[k].has_load = true;
        } else if (in->op == NYIR_STORE_I64 && !slots[k].has_store) {
          slots[k].store_tmpl = *in;
          slots[k].has_store = true;
        }
        continue;
      }
      if (nyr_inst_effects(in) &
          (NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_WRITE_MEMORY))
        return 0;
    }
  }
  if (!slot_len)
    return 0;
  for (size_t k = 0; k < slot_len; ++k) {
    lgp_slot_t *sl = &slots[k];
    if (!sl->has_load) {
      /*
       * Store-only slot: the loaded word has the stored value's class.
       */
      sl->load_tmpl = sl->store_tmpl;
      sl->load_tmpl.op = NYIR_LOAD_I64;
    }
    if (!sl->has_store) {
      sl->store_tmpl = sl->load_tmpl;
      sl->store_tmpl.op = NYIR_STORE_I64;
    }
  }

  /*
   * Dedicated preheader.
   */
  size_t preheader = SIZE_MAX;
  for (size_t k = cfg->pred_offsets[header]; k < cfg->pred_offsets[header + 1];
       ++k) {
    size_t p = cfg->pred_blocks[k];
    if (member[p])
      continue;
    if (preheader != SIZE_MAX)
      return 0;
    preheader = p;
  }
  if (preheader == SIZE_MAX ||
      cfg->succ_offsets[preheader + 1] - cfg->succ_offsets[preheader] != 1)
    return 0;

  /*
   * Dedicated exits.
   */
  size_t exits[LGP_MAX_EXITS];
  size_t exit_len = 0;
  for (size_t b = 0; b < nb; ++b) {
    if (!member[b])
      continue;
    for (size_t k = cfg->succ_offsets[b]; k < cfg->succ_offsets[b + 1]; ++k) {
      size_t e = cfg->succ_blocks[k];
      if (member[e])
        continue;
      bool seen = false;
      for (size_t x = 0; x < exit_len; ++x)
        seen |= exits[x] == e;
      if (seen)
        continue;
      for (size_t q = cfg->pred_offsets[e]; q < cfg->pred_offsets[e + 1]; ++q)
        if (!member[cfg->pred_blocks[q]])
          return 0;
      if (exit_len == LGP_MAX_EXITS)
        return 0;
      exits[exit_len++] = e;
    }
  }
  if (!exit_len)
    return 0;

  size_t next_local = lgp_next_local(f);
  for (size_t k = 0; k < slot_len; ++k) {
    slots[k].symbol = nyr_func_own_symbol_copy(f, slots[k].symbol);
    if (!slots[k].symbol)
      return -1;
    slots[k].local = (int)next_local++;
  }

  /*
   * Rewrite in-loop accesses in place.
   */
  for (size_t b = 0; b < nb; ++b) {
    if (!member[b])
      continue;
    for (size_t i = cfg->block_start[b]; i < cfg->block_end[b]; ++i) {
      nyr_inst_t *in = &f->data[i];
      if (in->op != NYIR_LOAD_I64 && in->op != NYIR_STORE_I64)
        continue;
      const char *sym = lgp_symbol_of(f, defs, in->a);
      size_t k = 0;
      while (k < slot_len && strcmp(slots[k].symbol, sym) != 0)
        ++k;
      if (in->op == NYIR_LOAD_I64) {
        in->op = NYIR_LOAD_LOCAL;
        in->a = -1;
      } else {
        in->op = NYIR_STORE_LOCAL;
        in->a = in->c;
        in->c = -1;
      }
      in->imm = slots[k].local;
      in->flags &=
          ~(NYIR_INST_F_MEM_BYTE | NYIR_INST_F_MEM_I32 | NYIR_INST_F_MEM_U16);
    }
  }

  /*
   * Preheader seeds and exit write-backs, inserted from the back.
   */
  size_t cap = slot_len * 3 * (1 + exit_len);
  lgp_insert_t *ins = calloc(cap, sizeof(*ins));
  if (!ins)
    return -1;
  size_t n = 0;
  size_t seed_at = lgp_block_tail(f, cfg, preheader);
  for (size_t k = 0; k < slot_len; ++k) {
    const lgp_slot_t *sl = &slots[k];
    int addr = f->next_value++, value = f->next_value++;
    nyr_inst_t a = lgp_clone(&sl->addr_tmpl, NYIR_ADDR_SYMBOL, addr, -1, -1,
                             sl->addr_tmpl.imm);
    a.symbol = sl->symbol;
    ins[n++] = (lgp_insert_t){seed_at, a};
    ins[n++] =
        (lgp_insert_t){seed_at, lgp_clone(&sl->load_tmpl, NYIR_LOAD_I64, value,
                                          addr, -1, sl->load_tmpl.imm)};
    ins[n++] =
        (lgp_insert_t){seed_at, lgp_clone(&sl->load_tmpl, NYIR_STORE_LOCAL, -1,
                                          value, -1, sl->local)};
  }
  for (size_t x = 0; x < exit_len; ++x) {
    size_t at = lgp_block_head(f, cfg, exits[x]);
    for (size_t k = 0; k < slot_len; ++k) {
      const lgp_slot_t *sl = &slots[k];
      int addr = f->next_value++, value = f->next_value++;
      ins[n++] = (lgp_insert_t){at, lgp_clone(&sl->load_tmpl, NYIR_LOAD_LOCAL,
                                              value, -1, -1, sl->local)};
      nyr_inst_t a = lgp_clone(&sl->addr_tmpl, NYIR_ADDR_SYMBOL, addr, -1, -1,
                               sl->addr_tmpl.imm);
      a.symbol = sl->symbol;
      ins[n++] = (lgp_insert_t){at, a};
      ins[n++] =
          (lgp_insert_t){at, lgp_clone(&sl->store_tmpl, NYIR_STORE_I64, -1,
                                       addr, value, sl->store_tmpl.imm)};
    }
  }
  /*
   * Insert back to front. Reversing first and then stable-sorting by
   * descending position makes each same-position group insert in reverse,
   * which leaves it in program order.
   */
  for (size_t i = 0; i < n / 2; ++i) {
    lgp_insert_t t = ins[i];
    ins[i] = ins[n - 1 - i];
    ins[n - 1 - i] = t;
  }
  for (size_t i = 1; i < n; ++i) {
    lgp_insert_t t = ins[i];
    size_t j = i;
    while (j > 0 && ins[j - 1].at < t.at) {
      ins[j] = ins[j - 1];
      --j;
    }
    ins[j] = t;
  }
  if (!nir_ensure_inst_space(f, n)) {
    free(ins);
    return -1;
  }
  for (size_t i = 0; i < n; ++i) {
    size_t at = ins[i].at;
    memmove(&f->data[at + 1], &f->data[at], (f->len - at) * sizeof(*f->data));
    f->data[at] = ins[i].inst;
    ++f->len;
  }
  free(ins);
  return 1;
}

bool nyr_loop_global_promote(nyr_func_t *f) {
  if (!f || !f->len || !nyr_has_loop(f) ||
      getenv("NYTRIX_NO_LOOP_GLOBAL_PROMOTE"))
    return true;
  for (int round = 0; round < LGP_MAX_ROUNDS; ++round) {
    nyr_cfg_t cfg = {0};
    if (!nyr_cfg_build(f, &cfg))
      return true;
    int *defs = nyr_build_defs(f);
    bool *member =
        calloc(cfg.block_count ? cfg.block_count : 1, sizeof(*member));
    int done = 0;
    for (size_t b = 0; defs && member && b < cfg.block_count && !done; ++b) {
      if (!cfg.reachable[b])
        continue;
      for (size_t k = cfg.succ_offsets[b]; k < cfg.succ_offsets[b + 1] && !done;
           ++k) {
        if (!cfg.backedge_edges[k])
          continue;
        size_t header = cfg.succ_blocks[k];
        if (!nyr_cfg_natural_loop_blocks(&cfg, b, header, member,
                                         cfg.block_count))
          continue;
        done = lgp_try_loop(f, &cfg, defs, header, member);
      }
    }
    free(member);
    free(defs);
    nyr_cfg_free(&cfg);
    if (done <= 0)
      return done == 0;
  }
  return true;
}
