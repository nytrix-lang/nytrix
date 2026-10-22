/*
 * Loop-invariant code motion over CFG-proven natural loops.
 */
#include "base/common.h"
#include "base/compat.h"
#include "base/debug.h"
#include "code/backend/ir/internal.h"
#include "code/backend/ir/opt/loop.h"
#include "code/backend/ir/opt/util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool nyr_licm_trace_enabled(void) {
  return ny_dbg_is_enabled(NY_DBG_HEURISTIC) ||
         ny_trace_enabled("NY_TRACE_LICM");
}

static bool licm_readonly_len_call(const nyr_inst_t *in) {
  return in && in->op == NYIR_CALL && in->symbol &&
         (strcmp(in->symbol, "rt_len") == 0 ||
          strcmp(in->symbol, "rt_tbuf_len_raw") == 0);
}

typedef struct {
  size_t index;
  nyr_inst_t inst;
  bool erase_original;
  bool is_guard;
  bool is_continuation_label;
} nyr_licm_move_t;

static bool licm_next_label(const nyr_func_t *f, int64_t *out) {
  if (!f || !out)
    return false;
  int64_t next = 0;
  for (size_t i = 0; i < f->len; ++i) {
    if (f->data[i].op != NYIR_LABEL || f->data[i].imm < next)
      continue;
    if (f->data[i].imm == INT64_MAX)
      return false;
    next = f->data[i].imm + 1;
  }
  *out = next;
  return true;
}

static bool licm_find_preheader(const nyr_cfg_t *cfg, const bool *in_loop,
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

static size_t licm_insert_position(const nyr_func_t *f, const nyr_cfg_t *cfg,
                                   size_t preheader) {
  size_t start = cfg->block_start[preheader];
  size_t end = cfg->block_end[preheader];
  /*
   * Conditional blocks may contain BR_IF followed by a fallback BR.  Insert
   * before the first terminator, never between the two terminators.
   */
  for (size_t i = start; i < end; ++i) {
    nyr_op_t op = f->data[i].op;
    if (op == NYIR_BR || op == NYIR_BR_IF || op == NYIR_RET)
      return i;
  }
  while (end > start && f->data[end - 1u].op == NYIR_NOP)
    --end;
  return end;
}

static bool licm_operand_invariant(const nyr_func_t *f, const nyr_cfg_t *cfg,
                                   const int *defs, size_t preheader,
                                   size_t insert_at, int value, int value_count,
                                   const bool *loop_defined,
                                   const bool *scheduled) {
  if (value < 0)
    return value == -1;
  if (value >= value_count)
    return false;
  if (loop_defined[value])
    return scheduled[value];
  int def = defs[value];
  if (def < 0 || (size_t)def >= f->len || !cfg->inst_block)
    return false;
  size_t def_block = cfg->inst_block[def];
  if (def_block == preheader)
    return (size_t)def < insert_at;
  return nyr_cfg_dominates(cfg, def_block, preheader);
}

/*
 * Extract independent range proofs for the divisor-zero and signed-overflow
 * traps. LICM may discharge the zero-divisor risk with an explicit guard.
 */
static bool licm_division_ranges(const nyr_func_t *f,
                                 const nyr_scev_info_t *scev, const int *defs,
                                 size_t at, const nyr_inst_t *in,
                                 const nyr_value_fact_t *facts,
                                 int value_count, bool *out_nonzero,
                                 bool *out_no_signed_overflow) {
  if (!in || !facts || value_count <= 0 || in->a < 0 || in->b < 0 ||
      in->a >= value_count || in->b >= value_count)
    return false;
  nyr_range_t numerator = facts[in->a].range;
  nyr_range_t denominator = facts[in->b].range;
  nyr_scev_affine_t affine = {0};
  if ((!numerator.has_min || !numerator.has_max) &&
      nyr_scev_affine_at(f, scev, defs, in->a, at, &affine) &&
      affine.range.has_min && affine.range.has_max)
    numerator = affine.range;
  memset(&affine, 0, sizeof(affine));
  if ((!denominator.has_min || !denominator.has_max) &&
      nyr_scev_affine_at(f, scev, defs, in->b, at, &affine) &&
      affine.range.has_min && affine.range.has_max)
    denominator = affine.range;
  bool nonzero = (denominator.has_min && denominator.min > 0) ||
                 (denominator.has_max && denominator.max < 0);
  bool no_signed_overflow = (numerator.has_min && numerator.min > INT64_MIN) ||
                            (denominator.has_min && denominator.min > -1) ||
                            (denominator.has_max && denominator.max < -1);
  if (out_nonzero)
    *out_nonzero = nonzero;
  if (out_no_signed_overflow)
    *out_no_signed_overflow = no_signed_overflow;
  return true;
}

static bool
licm_division_nontrapping(const nyr_func_t *f, const nyr_scev_info_t *scev,
                          const int *defs, size_t at, const nyr_inst_t *in,
                          const nyr_value_fact_t *facts, int value_count) {
  bool nonzero = false, no_signed_overflow = false;
  return licm_division_ranges(f, scev, defs, at, in, facts, value_count,
                              &nonzero, &no_signed_overflow) &&
         nonzero && no_signed_overflow;
}

static size_t licm_label_block(const nyr_cfg_t *cfg, int64_t label) {
  if (!cfg)
    return SIZE_MAX;
  for (size_t block = 0; block < cfg->block_count; ++block)
    if (cfg->block_label[block] == label)
      return block;
  return SIZE_MAX;
}

static bool licm_is_terminal_exit(const nyr_func_t *f, const nyr_cfg_t *cfg,
                                  const bool *in_loop, size_t block) {
  if (!f || !cfg || !in_loop || block >= cfg->block_count || in_loop[block] ||
      cfg->block_start[block] >= cfg->block_end[block])
    return false;
  size_t start = cfg->block_start[block];
  size_t end = cfg->block_end[block];
  if (f->data[end - 1u].op != NYIR_RET)
    return false;
  for (size_t i = start; i < end; ++i)
    if (f->data[i].op == NYIR_BR || f->data[i].op == NYIR_BR_IF)
      return false;
  return true;
}

/*
 * A zero-divisor check at the top of a loop makes an otherwise trapping
 * invariant division safe to hoist: clone the check into a dedicated
 * preheader, then label its fallthrough so header PHIs keep the right
 * predecessor after the preheader is split.
 */
static bool licm_find_zero_division_guard(
    const nyr_func_t *f, const nyr_cfg_t *cfg, const bool *in_loop,
    const int *defs, const nyr_value_fact_t *facts, int value_count,
    const bool *loop_defined, const bool *scheduled, size_t header,
    size_t preheader, size_t div_block, int divisor, size_t division_at,
    size_t insert_at, nyr_inst_t *out_guard, size_t *out_guard_index) {
  if (!f || !cfg || !in_loop || !defs || !facts || !loop_defined ||
      !scheduled || !out_guard || !out_guard_index ||
      header >= cfg->block_count || preheader >= cfg->block_count ||
      div_block >= cfg->block_count || !in_loop[header] ||
      !in_loop[div_block] || in_loop[preheader] || divisor < 0 ||
      divisor >= value_count)
    return false;
  size_t pred_start = cfg->pred_offsets[header];
  size_t pred_end = cfg->pred_offsets[header + 1u];
  size_t succ_start = cfg->succ_offsets[preheader];
  size_t succ_end = cfg->succ_offsets[preheader + 1u];
  if (pred_end - pred_start < 1u || succ_end - succ_start != 1u ||
      cfg->succ_blocks[succ_start] != header)
    return false;

  size_t block_start = cfg->block_start[header];
  size_t block_end = cfg->block_end[header];
  for (size_t i = block_start; i < block_end; ++i) {
    const nyr_inst_t *branch = &f->data[i];
    if (branch->op != NYIR_BR_IF)
      continue;
    if (branch->a < 0 || branch->a >= value_count)
      continue;
    int condition_def = defs[branch->a];
    if (condition_def < 0 || (size_t)condition_def >= f->len)
      continue;
    size_t condition_block = cfg->inst_block[condition_def];
    if (condition_block >= cfg->block_count)
      continue;

    const nyr_inst_t *comparison = &f->data[condition_def];
    if (comparison->op != NYIR_CMP_I64 || comparison->cmp != NYIR_CMP_EQ ||
        (comparison->a != divisor && comparison->b != divisor))
      continue;
    if (in_loop[condition_block]) {
      if (condition_block != header || !loop_defined[branch->a] ||
          !scheduled[branch->a] ||
          !licm_operand_invariant(f, cfg, defs, preheader, insert_at,
                                  comparison->a, value_count, loop_defined,
                                  scheduled) ||
          !licm_operand_invariant(f, cfg, defs, preheader, insert_at,
                                  comparison->b, value_count, loop_defined,
                                  scheduled))
        continue;
    } else if (condition_block == preheader
                   ? (size_t)condition_def >= insert_at
                   : !nyr_cfg_dominates(cfg, condition_block, preheader)) {
      continue;
    }
    int zero_value = comparison->a == divisor ? comparison->b : comparison->a;
    if (zero_value < 0 || zero_value >= value_count ||
        !facts[zero_value].known_const || facts[zero_value].const_value != 0)
      continue;

    size_t panic_block = licm_label_block(cfg, branch->imm);
    if (!licm_is_terminal_exit(f, cfg, in_loop, panic_block))
      continue;
    size_t succ_begin = cfg->succ_offsets[header];
    size_t header_succ_end = cfg->succ_offsets[header + 1u];
    if (header_succ_end - succ_begin != 2u)
      continue;
    bool found_panic_edge = false, found_division_edge = false;
    for (size_t edge = succ_begin; edge < header_succ_end; ++edge) {
      size_t successor = cfg->succ_blocks[edge];
      if (successor == panic_block) {
        found_panic_edge = true;
        continue;
      }
      size_t successor_start = cfg->block_start[successor];
      size_t successor_end = cfg->block_end[successor];
      while (successor_start < successor_end &&
             f->data[successor_start].op == NYIR_NOP)
        ++successor_start;
      if (successor_start < successor_end &&
          f->data[successor_start].op == NYIR_BR &&
          licm_label_block(cfg, f->data[successor_start].imm) == div_block)
        found_division_edge = true;
    }
    if (!found_panic_edge || !found_division_edge)
      continue;

    bool safe_prefix = true;
    for (size_t prefix = block_start; prefix < i; ++prefix) {
      const nyr_inst_t *in = &f->data[prefix];
      if (in->op == NYIR_NOP || nyr_effective_effects(in) == NYIR_EFFECT_NONE)
        continue;
      if (in->dst < 0 || in->dst >= value_count || !loop_defined[in->dst] ||
          !scheduled[in->dst]) {
        safe_prefix = false;
        break;
      }
    }
    for (size_t prefix = cfg->block_start[div_block];
         safe_prefix && prefix < division_at; ++prefix) {
      const nyr_inst_t *in = &f->data[prefix];
      if (in->op == NYIR_NOP || nyr_effective_effects(in) == NYIR_EFFECT_NONE)
        continue;
      if (in->dst < 0 || in->dst >= value_count || !loop_defined[in->dst] ||
          !scheduled[in->dst]) {
        safe_prefix = false;
        break;
      }
    }
    if (!safe_prefix)
      continue;

    *out_guard = *branch;
    *out_guard_index = i;
    return true;
  }
  return false;
}

/*
 * A direct symbol address denotes a linker-resolved global storage slot.
 * Reading that slot cannot fault merely because a loop takes zero trips.
 * Keep this proof deliberately narrow: computed pointers and byte payload
 * accesses retain the ordinary may-trap rule.
 */
static bool licm_direct_symbol_slot_address(const nyr_func_t *f,
                                            const int *defs, int value,
                                            unsigned depth) {
  if (!f || !defs || value < 0 || value >= f->next_value || depth > 8 ||
      defs[value] < 0 || (size_t)defs[value] >= f->len)
    return false;
  const nyr_inst_t *def = &f->data[defs[value]];
  if (def->op == NYIR_ADDR_SYMBOL)
    return def->symbol && *def->symbol;
  if (def->op == NYIR_COPY)
    return licm_direct_symbol_slot_address(f, defs, def->a, depth + 1);
  return false;
}

static bool licm_memory_op(nyr_op_t op) {
  switch (op) {
  case NYIR_LOAD_I64:
  case NYIR_STORE_I64:
  case NYIR_VEC4_LOAD_F64:
  case NYIR_VEC4_STORE_F64:
  case NYIR_VEC8_LOAD_F32:
  case NYIR_VEC8_STORE_F32:
  case NYIR_VEC4_LOAD_I64:
  case NYIR_VEC4_STORE_I64:
  case NYIR_VEC8_LOAD_I64:
  case NYIR_VEC8_STORE_I64:
    return true;
  default:
    return false;
  }
}

/*
 * Keep memory address expressions in the loop that owns their access. A
 * later vectorization pass can rewrite scalar accesses while depending on
 * their loop-local address shape; hoisting the scalar arithmetic first can
 * invalidate native register-home tracking for the rewritten access.
 */
static bool licm_memory_address_component(const nyr_func_t *f, int value,
                                          unsigned depth) {
  if (!f || value < 0 || depth > 12)
    return false;
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *user = &f->data[i];
    if (user->op == NYIR_NOP ||
        (user->a != value && user->b != value && user->c != value &&
         user->d != value && user->e != value && user->f != value))
      continue;
    if (licm_memory_op(user->op) && user->a == value)
      return true;
    if (user->dst >= 0 &&
        (user->op == NYIR_ADD_I64 || user->op == NYIR_SUB_I64 ||
         user->op == NYIR_SHL_I64 || user->op == NYIR_MUL_I64 ||
         user->op == NYIR_COPY) &&
        licm_memory_address_component(f, user->dst, depth + 1))
      return true;
  }
  return false;
}

static bool licm_candidate(const nyr_func_t *f, const nyr_scev_info_t *scev,
                           const int *defs, size_t at, const nyr_inst_t *in,
                           bool loop_writes_memory, bool stable_managed_read,
                           bool trap_equivalent_read,
                           const nyr_value_fact_t *facts,
                           const bool *stored_local, size_t local_count,
                           const bool *addr_taken_local, int value_count,
                           const bool *loop_defined, const bool *scheduled,
                           const nyr_cfg_t *cfg, const bool *in_loop,
                           size_t header, size_t preheader, size_t insert_at,
                           nyr_inst_t *out_guard, size_t *out_guard_index) {
  if (!in)
    return false;
  if (out_guard)
    memset(out_guard, 0, sizeof(*out_guard));
  if (out_guard_index)
    *out_guard_index = SIZE_MAX;
  /* Division can trap; require range proof or a cloned zero-divisor guard. */
  bool division_nontrapping =
      (in->op == NYIR_DIV_I64 || in->op == NYIR_MOD_I64) &&
      licm_division_nontrapping(f, scev, defs, at, in, facts, value_count);
  bool guarded_division = false;
  if ((in->op == NYIR_DIV_I64 || in->op == NYIR_MOD_I64) &&
      !division_nontrapping && out_guard && out_guard_index) {
    bool nonzero = false, no_signed_overflow = false;
    if (licm_division_ranges(f, scev, defs, at, in, facts, value_count,
                             &nonzero, &no_signed_overflow) &&
        !nonzero && no_signed_overflow) {
      size_t div_block = at < f->len ? cfg->inst_block[at] : SIZE_MAX;
      guarded_division = licm_find_zero_division_guard(
          f, cfg, in_loop, defs, facts, value_count, loop_defined, scheduled,
          header, preheader, div_block, in->b, at, insert_at, out_guard,
          out_guard_index);
    }
  }
  if ((in->op == NYIR_DIV_I64 || in->op == NYIR_MOD_I64) &&
      !division_nontrapping && !guarded_division) {
    if (nyr_licm_trace_enabled())
      ny_dbg_heuristic_event(
          "LICM",
          "reject invariant @%zu op=%d dst=v%d: divisor-zero or "
          "signed-overflow trap is not disproven",
          at, (int)in->op, in->dst);
    return false;
  }
  if (in->op == NYIR_BOUNDS_CHECK) {
    if (!trap_equivalent_read)
      return false;
    const int inputs[] = {in->a, in->b, in->c};
    for (size_t k = 0; k < sizeof(inputs) / sizeof(inputs[0]); ++k)
      if (!licm_operand_invariant(f, cfg, defs, preheader, insert_at, inputs[k],
                                  value_count, loop_defined, scheduled))
        return false;
    return true;
  }
  if (in->dst < 0)
    return false;
  unsigned effects = nyr_effective_effects(in);
  bool direct_symbol_slot_read =
      in->op == NYIR_LOAD_I64 && !(in->flags & NYIR_INST_F_MEM_BYTE) &&
      licm_direct_symbol_slot_address(f, defs, in->a, 0);
  const unsigned forbidden =
      NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_WRITE_LOCAL | NYIR_EFFECT_CONTROL |
      NYIR_EFFECT_VOLATILE | NYIR_EFFECT_ALLOCATION |
      NYIR_EFFECT_UNKNOWN_SIDE_EFFECT | NYIR_EFFECT_IO | NYIR_EFFECT_THREAD |
      NYIR_EFFECT_FFI | NYIR_EFFECT_FENV;
  if (effects & forbidden)
    return false;
  if ((effects & NYIR_EFFECT_MAY_TRAP) && !trap_equivalent_read &&
      !division_nontrapping && !guarded_division && !direct_symbol_slot_read)
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
      (size_t)in->imm < local_count &&
      ((stored_local && stored_local[in->imm]) ||
       (addr_taken_local && addr_taken_local[in->imm])))
    return false;
  if (in->op == NYIR_ADD_I64 && in->dst >= 0 &&
      licm_memory_address_component(f, in->dst, 0))
    return false;
  const int inputs[] = {in->a, in->b, in->c, in->d, in->e, in->f};
  for (size_t k = 0; k < sizeof(inputs) / sizeof(inputs[0]); ++k)
    if (!licm_operand_invariant(f, cfg, defs, preheader, insert_at, inputs[k],
                                value_count, loop_defined, scheduled))
      return false;
  for (size_t k = 0; k < in->extra_args_len; ++k)
    if (!licm_operand_invariant(f, cfg, defs, preheader, insert_at,
                                in->extra_args[k], value_count, loop_defined,
                                scheduled))
      return false;
  return true;
}

/*
 * A read-only helper that may fault on an invalid raw pointer can be moved
 * into the preheader only when doing so is trap-equivalent. A call in the
 * header executes on initial entry even for a zero-trip loop, provided the
 * preheader has no bypass edge. Other in-loop reads require a statically
 * positive trip count and dominance of the latch and every non-header exit.
 */
static bool licm_trap_equivalent_read(const nyr_cfg_t *cfg,
                                      const bool *in_loop, size_t header,
                                      size_t latch, size_t preheader,
                                      size_t call_block, bool is_call,
                                      bool positive_trip) {
  if (!cfg || !in_loop || header >= cfg->block_count ||
      latch >= cfg->block_count || preheader >= cfg->block_count ||
      call_block >= cfg->block_count)
    return false;
  /*
   * A call in the header executes on initial entry even for a zero-trip
   * loop. Hoist it only through a dedicated preheader, so no bypass path
   * gains a new trapping operation.
   */
  if (is_call && call_block == header) {
    size_t begin = cfg->succ_offsets[preheader];
    size_t end = cfg->succ_offsets[preheader + 1u];
    return end == begin + 1u && cfg->succ_blocks[begin] == header;
  }
  if (!positive_trip || !nyr_cfg_dominates(cfg, call_block, latch))
    return false;
  for (size_t block = 0; block < cfg->block_count; ++block) {
    if (!in_loop[block] || block == header)
      continue;
    for (size_t e = cfg->succ_offsets[block]; e < cfg->succ_offsets[block + 1];
         ++e) {
      size_t succ = cfg->succ_blocks[e];
      if (!in_loop[succ] && !nyr_cfg_dominates(cfg, call_block, block))
        return false;
    }
  }
  return true;
}

static bool licm_addr_from_tbuf_data(const nyr_func_t *f,
                                     const nyr_value_fact_t *facts,
                                     const int *defs, int addr, int base,
                                     unsigned depth) {
  if (!f || !facts || !defs || addr < 0 || base < 0 || depth > 16)
    return false;
  if (addr == base)
    return true;
  if (addr >= f->next_value || defs[addr] < 0 || (size_t)defs[addr] >= f->len)
    return false;
  const nyr_inst_t *in = &f->data[defs[addr]];
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
  const nyr_value_fact_t *fact = &facts[offset];
  return fact->range.has_min && fact->range.min >= 0;
}

/*
 * rt_tbuf_len_raw reads only the count word at data[-24].  Element stores
 * through data + nonnegative_offset cannot modify that header.  This lets
 * dense update loops hoist one invariant length read even though the loop
 * legitimately writes the buffer payload.  Any unknown write/call, negative
 * pointer arithmetic, or store through another base remains a hard barrier.
 */
static bool licm_tbuf_len_header_stable(const nyr_func_t *f,
                                        const nyr_cfg_t *cfg,
                                        const bool *in_loop,
                                        const nyr_value_fact_t *facts,
                                        const int *defs, int base) {
  if (!f || !cfg || !in_loop || !facts || !defs || base < 0)
    return false;
  for (size_t block = 0; block < cfg->block_count; ++block) {
    if (!in_loop[block])
      continue;
    for (size_t i = cfg->block_start[block]; i < cfg->block_end[block]; ++i) {
      const nyr_inst_t *in = &f->data[i];
      unsigned effects = nyr_effective_effects(in);
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

static bool licm_grow_moves(nyr_licm_move_t **moves, size_t *cap,
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
  nyr_licm_move_t *grown = realloc(*moves, next * sizeof(*grown));
  if (!grown)
    return false;
  *moves = grown;
  *cap = next;
  return true;
}
/*
 * A loop instruction may be visited before its invariant operand when CFG
 * block order differs from dominance order. Emit selected instructions in
 * dependency order so moving them cannot invert an SSA definition and use.
 */
static bool licm_order_moves(nyr_licm_move_t *moves, size_t move_count) {
  if (!moves || move_count < 2)
    return true;
  if (move_count > SIZE_MAX / sizeof(bool) ||
      move_count > SIZE_MAX / sizeof(size_t) ||
      move_count > SIZE_MAX / sizeof(*moves)) {
    return false;
  }
  bool *done = calloc(move_count, sizeof(*done));
  size_t *order = malloc(move_count * sizeof(*order));
  nyr_licm_move_t *ordered = malloc(move_count * sizeof(*ordered));
  if (!done || !order || !ordered) {
    free(done);
    free(order);
    free(ordered);
    return false;
  }
  for (size_t out = 0; out < move_count; ++out) {
    size_t pick = SIZE_MAX;
    for (size_t i = 0; i < move_count; ++i) {
      if (done[i])
        continue;
      bool ready = true;
      if (moves[i].is_continuation_label) {
        for (size_t dep = 0; dep < i; ++dep)
          if (moves[dep].is_guard && !done[dep]) {
            ready = false;
            break;
          }
      }
      const int inputs[] = {moves[i].inst.a, moves[i].inst.b, moves[i].inst.c,
                            moves[i].inst.d, moves[i].inst.e, moves[i].inst.f};
      for (size_t k = 0; k < sizeof(inputs) / sizeof(inputs[0]); ++k) {
        if (inputs[k] < 0)
          continue;
        for (size_t dep = 0; dep < move_count; ++dep) {
          if (moves[dep].inst.dst == inputs[k] && !done[dep]) {
            ready = false;
            break;
          }
        }
        if (!ready)
          break;
      }
      if (ready) {
        for (size_t k = 0; k < moves[i].inst.extra_args_len; ++k) {
          int input = moves[i].inst.extra_args[k];
          for (size_t dep = 0; dep < move_count; ++dep) {
            if (moves[dep].inst.dst == input && !done[dep]) {
              ready = false;
              break;
            }
          }
          if (!ready)
            break;
        }
      }

      if (ready) {
        pick = i;
        break;
      }
    }
    if (pick == SIZE_MAX) {
      free(done);
      free(order);
      free(ordered);
      return false;
    }
    order[out] = pick;
    done[pick] = true;
  }
  for (size_t i = 0; i < move_count; ++i)
    ordered[i] = moves[order[i]];
  memcpy(moves, ordered, move_count * sizeof(*moves));
  free(done);
  free(order);
  free(ordered);
  return true;
}

/*
 * Hoist one natural loop's currently provable invariants.  Returning after a
 * single CFG mutation lets the caller rebuild dominance/topology before
 * considering another loop, so nested/adjacent loops never consume stale
 * block or instruction indices.
 */
static bool licm_hoist_one(nyr_func_t *f, bool *changed) {
  if (changed)
    *changed = false;
  nyr_cfg_t cfg = {0};
  nyr_scev_info_t scev = {0};
  if (!nyr_cfg_build(f, &cfg) || !nyr_scev_analyze(f, &scev)) {
    nyr_cfg_free(&cfg);
    nyr_scev_free(&scev);
    return false;
  }
  bool *in_loop = calloc(cfg.block_count, sizeof(*in_loop));
  if (!in_loop) {
    nyr_cfg_free(&cfg);
    nyr_scev_free(&scev);
    return false;
  }

  for (size_t latch = 0; latch < cfg.block_count; ++latch) {
    for (size_t e = cfg.succ_offsets[latch]; e < cfg.succ_offsets[latch + 1];
         ++e) {
      size_t header = cfg.succ_blocks[e];
      if (!nyr_cfg_is_backedge(&cfg, latch, header) ||
          !nyr_cfg_natural_loop_blocks(&cfg, latch, header, in_loop,
                                        cfg.block_count))
        continue;
      size_t preheader = SIZE_MAX;
      if (!licm_find_preheader(&cfg, in_loop, header, &preheader)) {
        if (nyr_licm_trace_enabled())
          ny_dbg_heuristic_event(
              "LICM",
              "reject loop header=b%zu latch=b%zu: expected exactly one "
              "outside predecessor for a safe hoist target",
              header, latch);
        continue;
      }
      size_t preheader_insert_at = licm_insert_position(f, &cfg, preheader);
      bool positive_trip = false;
      for (size_t si = 0; si < scev.count; ++si) {
        const nyr_scev_loop_t *sl = &scev.loops[si];
        if (sl->header_block == header && sl->latch_block == latch &&
            sl->trip_count_known && sl->trip_count > 0) {
          positive_trip = true;
          break;
        }
      }
      if (nyr_licm_trace_enabled())
        ny_dbg_heuristic_event(
            "LICM",
            "loop header=b%zu latch=b%zu preheader=b%zu: positive-trip=%s; "
            "entry/trip proofs gate trap-equivalent reads",
            header, latch, preheader, positive_trip ? "yes" : "no");

      /*
       * Keep the call/raw-memory barrier unless the loop's only call is the
       * audited rt_len read and all explicit memory operations are loads.
       */
      bool loop_has_call = false, loop_has_memory_access = false;
      bool loop_has_memory_write = false, loop_calls_only_len = true;
      for (size_t block = 0; block < cfg.block_count; ++block) {
        if (!in_loop[block])
          continue;
        for (size_t i = cfg.block_start[block]; i < cfg.block_end[block]; ++i) {
          const nyr_inst_t *in = &f->data[i];
          if (in->op == NYIR_CALL) {
            loop_has_call = true;
            loop_calls_only_len &= licm_readonly_len_call(in);
          }
          switch (in->op) {
          case NYIR_LOAD_I64:
          case NYIR_VEC4_LOAD_I64:
          case NYIR_VEC8_LOAD_I64:
          case NYIR_VEC4_LOAD_F64:
          case NYIR_VEC8_LOAD_F32:
            loop_has_memory_access = true;
            break;
          case NYIR_STORE_I64:
          case NYIR_VEC4_STORE_I64:
          case NYIR_VEC8_STORE_I64:
            loop_has_memory_access = true;
            loop_has_memory_write = true;
            break;
          default:
            break;
          }
        }
      }
      if (loop_has_call && loop_has_memory_access &&
          (!loop_calls_only_len || loop_has_memory_write))
        continue;

      size_t local_count = nyr_max_local(f);
      bool *stored_local =
          local_count ? calloc(local_count, sizeof(*stored_local)) : NULL;
      bool *addr_taken_local =
          local_count ? calloc(local_count, sizeof(*addr_taken_local)) : NULL;
      bool *loop_defined = calloc((size_t)f->next_value, sizeof(*loop_defined));
      bool *scheduled = calloc((size_t)f->next_value, sizeof(*scheduled));
      bool *selected_inst = calloc(f->len, sizeof(*selected_inst));
      nyr_value_fact_t *facts = calloc((size_t)f->next_value, sizeof(*facts));
      int *defs = malloc((size_t)f->next_value * sizeof(*defs));
      if ((local_count && (!stored_local || !addr_taken_local)) ||
          !loop_defined || !scheduled || !selected_inst || !facts || !defs ||
          !nyr_analyze_values(f, facts, (size_t)f->next_value, NULL, 0)) {
        free(stored_local);
        free(addr_taken_local);
        free(loop_defined);
        free(scheduled);
        free(selected_inst);
        free(facts);
        free(defs);
        free(in_loop);
        nyr_cfg_free(&cfg);
        nyr_scev_free(&scev);
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
          const nyr_inst_t *in = &f->data[i];
          unsigned effects = nyr_effective_effects(in);
          /*
           * Calls and externally observable effects can mutate memory
           * reachable through globals or arguments. Their current summaries
           * do not identify every target, so require a no-clobber loop before
           * moving any read across its boundary.
           */
          const unsigned memory_clobbers =
              NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_IO | NYIR_EFFECT_THREAD |
              NYIR_EFFECT_FFI | NYIR_EFFECT_VOLATILE |
              NYIR_EFFECT_UNKNOWN_SIDE_EFFECT;
          if ((effects & memory_clobbers) ||
              (effects & NYIR_EFFECT_ALLOCATION) ||
              (in->op == NYIR_CALL && !licm_readonly_len_call(in)))
            loop_writes_memory = true;
          if (in->op == NYIR_STORE_LOCAL && in->imm >= 0 &&
              (size_t)in->imm < local_count)
            stored_local[in->imm] = true;
          if (in->op == NYIR_ADDR_LOCAL && in->imm >= 0 &&
              (size_t)in->imm < local_count)
            addr_taken_local[in->imm] = true;
          if (in->dst >= 0 && in->dst < f->next_value)
            loop_defined[in->dst] = true;
        }
      }

      nyr_licm_move_t *moves = NULL;
      size_t move_count = 0, move_cap = 0;
      bool progress;
      do {
        progress = false;
        for (size_t block = 0; block < cfg.block_count; ++block) {
          if (!in_loop[block])
            continue;
          for (size_t i = cfg.block_start[block]; i < cfg.block_end[block];
               ++i) {
            nyr_inst_t *in = &f->data[i];
            unsigned effects = nyr_effective_effects(in);
            bool is_may_trap = (effects & NYIR_EFFECT_MAY_TRAP) != 0;
            bool trap_equivalent_read =
                is_may_trap && licm_trap_equivalent_read(
                                   &cfg, in_loop, header, latch, preheader,
                                   block, in->op == NYIR_CALL, positive_trip);
            bool stable_managed_read =
                in->op == NYIR_CALL && in->symbol &&
                strcmp(in->symbol, "rt_tbuf_len_raw") == 0 && in->a >= 0 &&
                licm_tbuf_len_header_stable(f, &cfg, in_loop, facts, defs,
                                            in->a);
            nyr_inst_t zero_division_guard = {0};
            size_t zero_division_guard_index = SIZE_MAX;
            bool candidate = licm_candidate(
                f, &scev, defs, i, in, loop_writes_memory, stable_managed_read,
                trap_equivalent_read, facts, stored_local, local_count,
                addr_taken_local, f->next_value, loop_defined, scheduled, &cfg,
                in_loop, header, preheader, preheader_insert_at,
                &zero_division_guard, &zero_division_guard_index);
            if (candidate && !selected_inst[i] && nyr_licm_trace_enabled())
              ny_dbg_heuristic_event(
                  "LICM",
                  "accept invariant @%zu op=%d dst=v%d from b%zu: "
                  "effects=0x%x; "
                  "loop-writes-memory=%s stable-managed-read=%s "
                  "trap-equivalent=%s",
                  i, (int)in->op, in->dst, block, effects,
                  loop_writes_memory ? "yes" : "no",
                  stable_managed_read ? "yes" : "no",
                  trap_equivalent_read ? "yes" : "no");
            if (selected_inst[i] || !candidate)
              continue;
            bool guard_already_queued = false;
            bool continuation_label_queued = false;
            bool different_guard_queued = false;
            if (zero_division_guard_index != SIZE_MAX) {
              for (size_t k = 0; k < move_count; ++k)
                if (moves[k].is_guard) {
                  if (moves[k].index == zero_division_guard_index)
                    guard_already_queued = true;
                  else
                    different_guard_queued = true;
                } else if (moves[k].is_continuation_label) {
                  continuation_label_queued = true;
                }
            }
            if (different_guard_queued)
              continue;
            int64_t continuation_label = -1;
            bool needs_continuation_label =
                zero_division_guard_index != SIZE_MAX &&
                !continuation_label_queued;
            if (needs_continuation_label &&
                !licm_next_label(f, &continuation_label))
              continue;
            size_t required_moves =
                1u +
                (zero_division_guard_index != SIZE_MAX && !guard_already_queued
                     ? 1u
                     : 0u) +
                (needs_continuation_label ? 1u : 0u);
            if (required_moves > SIZE_MAX - move_count ||
                !licm_grow_moves(&moves, &move_cap,
                                 move_count + required_moves)) {
              free(moves);
              free(stored_local);
              free(addr_taken_local);
              free(loop_defined);
              free(scheduled);
              free(selected_inst);
              free(facts);
              free(defs);
              free(in_loop);
              nyr_cfg_free(&cfg);
              nyr_scev_free(&scev);
              return false;
            }
            if (zero_division_guard_index != SIZE_MAX &&
                !guard_already_queued) {
              size_t guard_insert_at = 0;
              while (guard_insert_at < move_count &&
                     moves[guard_insert_at].is_guard)
                ++guard_insert_at;
              memmove(&moves[guard_insert_at + 1u], &moves[guard_insert_at],
                      (move_count - guard_insert_at) * sizeof(*moves));
              moves[guard_insert_at] =
                  (nyr_licm_move_t){.index = zero_division_guard_index,
                                     .inst = zero_division_guard,
                                     .erase_original = true,
                                     .is_guard = true};
              ++move_count;
            }
            if (needs_continuation_label) {
              size_t label_insert_at = 0;
              while (label_insert_at < move_count &&
                     moves[label_insert_at].is_guard)
                ++label_insert_at;
              memmove(&moves[label_insert_at + 1u], &moves[label_insert_at],
                      (move_count - label_insert_at) * sizeof(*moves));
              moves[label_insert_at] =
                  (nyr_licm_move_t){.index = SIZE_MAX,
                                     .inst = {.op = NYIR_LABEL,
                                              .dst = -1,
                                              .a = -1,
                                              .b = -1,
                                              .c = -1,
                                              .d = -1,
                                              .e = -1,
                                              .f = -1,
                                              .imm = continuation_label},
                                     .is_continuation_label = true};
              ++move_count;
            }
            moves[move_count++] = (nyr_licm_move_t){
                .index = i, .inst = *in, .erase_original = true};
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
        free(addr_taken_local);
        free(loop_defined);
        free(scheduled);
        free(selected_inst);
        free(facts);
        free(defs);
        continue;
      }

      /*
       * May-trap moves (raw loads, bounds checks, audited read calls)
       * may only hoist into a DEDICATED preheader whose single successor
       * is the loop header. A preheader that also has a bypass edge
       * would execute the trapping op even when the loop is skipped.
       * IRCE's own hoister enforces exactly this discipline.
       */
      {
        bool dedicated = false;
        if (preheader < cfg.block_count) {
          size_t s0 = cfg.succ_offsets[preheader];
          size_t s1 = cfg.succ_offsets[preheader + 1];
          dedicated = (s1 - s0) == 1 && cfg.succ_blocks[s0] == header;
        }
        if (!dedicated) {
          size_t before = move_count;
          size_t w = 0;
          for (size_t k = 0; k < move_count; ++k) {
            if (!(nyr_effective_effects(&moves[k].inst) &
                  NYIR_EFFECT_MAY_TRAP))
              moves[w++] = moves[k];
          }
          move_count = w;
          if (before != move_count && nyr_licm_trace_enabled())
            ny_dbg_heuristic_event(
                "LICM",
                "reject %zu may-trap hoists from b%zu: preheader is not "
                "dedicated to header b%zu, so a loop-bypass path could gain "
                "a trap",
                before - move_count, preheader, header);
        }
      }
      if (move_count == 0) {
        free(moves);
        free(stored_local);
        free(addr_taken_local);
        free(loop_defined);
        free(scheduled);
        free(selected_inst);
        free(facts);
        free(defs);
        continue;
      }

      if (!licm_order_moves(moves, move_count)) {
        free(moves);
        free(stored_local);
        free(addr_taken_local);
        free(loop_defined);
        free(scheduled);
        free(selected_inst);
        free(facts);
        free(defs);
        free(in_loop);
        nyr_cfg_free(&cfg);
        nyr_scev_free(&scev);
        return false;
      }

      size_t at = licm_insert_position(f, &cfg, preheader);
      if (nyr_licm_trace_enabled())
        ny_dbg_heuristic_event(
            "LICM",
            "insert %zu instruction(s) for loop header=b%zu into b%zu at "
            "instruction %zu",
            move_count, header, preheader, at);
      if (move_count > SIZE_MAX - f->len) {
        free(moves);
        free(stored_local);
        free(addr_taken_local);
        free(loop_defined);
        free(scheduled);
        free(selected_inst);
        free(facts);
        free(defs);
        free(in_loop);
        nyr_cfg_free(&cfg);
        nyr_scev_free(&scev);
        return false;
      }
      size_t need = f->len + move_count;
      if (!nir_ensure_inst_space(f, move_count)) {
        free(moves);
        free(stored_local);
        free(addr_taken_local);
        free(loop_defined);
        free(scheduled);
        free(selected_inst);
        free(facts);
        free(defs);
        free(in_loop);
        nyr_cfg_free(&cfg);
        nyr_scev_free(&scev);
        return false;
      }

      int64_t old_preheader_label = cfg.block_label[preheader];
      for (size_t k = 0; k < move_count; ++k) {
        if (!moves[k].is_continuation_label)
          continue;
        int64_t continuation = moves[k].inst.imm;
        for (size_t i = cfg.block_start[header]; i < cfg.block_end[header];
             ++i) {
          nyr_inst_t *in = &f->data[i];
          if (in->op != NYIR_PHI)
            continue;
          for (size_t edge = 0; edge < in->phi_incoming_len; ++edge)
            if (in->phi_incoming[edge].predecessor_label == old_preheader_label)
              in->phi_incoming[edge].predecessor_label = continuation;
        }
      }

      /*
       * Transfer ownership of instruction metadata to the preheader copies.
       */
      for (size_t k = 0; k < move_count; ++k) {
        if (!moves[k].erase_original)
          continue;
        size_t i = moves[k].index;
        f->data[i] = (nyr_inst_t){.op = NYIR_NOP,
                                   .dst = -1,
                                   .a = -1,
                                   .b = -1,
                                   .c = -1,
                                   .d = -1,
                                   .e = -1,
                                   .f = -1};
      }
      memmove(&f->data[at + move_count], &f->data[at],
              (f->len - at) * sizeof(*f->data));
      for (size_t k = 0; k < move_count; ++k)
        f->data[at + k] = moves[k].inst;
      f->len = need;

      free(moves);
      free(stored_local);
      free(addr_taken_local);
      free(loop_defined);
      free(scheduled);
      free(selected_inst);
      free(facts);
      free(defs);
      free(in_loop);
      nyr_cfg_free(&cfg);
      nyr_scev_free(&scev);
      if (changed)
        *changed = true;
      return true;
    }
  }

  free(in_loop);
  nyr_cfg_free(&cfg);
  nyr_scev_free(&scev);
  return true;
}

bool nyr_licm(nyr_func_t *f) {
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
