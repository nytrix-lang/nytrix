/*
 * x86-64 object encoder: machine-form -> x64 instruction encoding
 * with ModR/M, SIB, REX, and VEX prefix handling for SSE and AVX.
 *
 * Every heap allocation here guards its allocation and frees on failure:
 * mach_param_slots/mach_spill_params, mach_add_patch/mach_add_reloc, the
 * encoder-owned color_seeded/fp/vec trios, and the stencil-fold + bundle
 * val/known/lab_at and order/results paths all free on OOM; no path leaks.
 * mach_cond_setcc() unifies int and float setcc opcode selection in one
 * lookup table keyed by {cond, is_float}.
 * color_seeded/fp/vec use stack-local bool[MACH_SEEDED_STACK_MAX] for
 * vreg_len ≤ 64 with heap calloc fallback for large functions, avoiding
 * per-encode heap traffic.
 * Target-specific object scaffolding stays local to preserve x64 relocation,
 * instruction-layout, and ISA encoding invariants.
 */
#include "base/debug.h"
#include "base/parallel.h"
#include "code/backend/abi.h"
#include "code/backend/ir/machine.h"
#include "code/backend/object/internal.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static bool mach_pow2_divisor(const nyr_mach_operand_t *op,
                              const int64_t *imm_fold,
                              const bool *imm_fold_known, size_t vreg_len,
                              unsigned *shift_out);
/*
 * machine form-owned x86-64 byte encoder for scalar machine form.
 *
 * This is the independence path: NYIR → machine form → bytes without
 * going through the NYIR object encoder. Matches the machine form text
 * emitter's stack-slot model so JIT and text-asm share semantics.
 */

typedef enum {
  MACH_CMP_KEEP = 0,
  MACH_CMP_SKIP_FOR_SELECT,
  MACH_CMP_REUSE_FLAGS,
} nyr_x64_cmp_action_t;

typedef struct {
  nyr_obj_buf_t code;
  const nyr_mach_func_t *mach;
  const ny_native_target_info_t *target;
  const char *debug_name;
  size_t *block_off;
  size_t *patch_at;
  uint32_t *patch_block;
  size_t patch_count;
  size_t patch_cap;
  nyr_x64_obj_reloc_t *relocs;
  size_t reloc_count;
  size_t reloc_cap;
  char *err;
  size_t err_len;
  /*
   * QBE-inspired sticky: rax may already hold a stack slot's value.
   */
  int last_rax_off;
  bool last_rax_valid;
  int pending_rax_off;
  bool pending_rax_valid;
  uint32_t r10_index_vreg;
  bool r10_index_valid;
  uint32_t pending_sib_vreg;
  unsigned pending_sib_base;
  unsigned pending_sib_index;
  unsigned pending_sib_scale;
  bool pending_sib_valid;
  nyr_mach_regalloc_t regalloc;
  size_t colors_len;
  size_t cur_inst;
  size_t cur_block;
  bool cur_block_valid;
  bool *color_seeded;
  /*
   * Dedicated callee-saved colors for loop PHIs and COPY snapshots.
   */
  int *loop_gpr;
  int *loop_fpr;
  uint8_t *loop_fpr_entry;
  bool no_sticky;         /* disable sticky rax (multi-block safety) */
  bool force_stack_reads; /* vector CFG joins use canonical scalar homes */
  /*
   * Trust allocator-carried GPR segments at block entry without an in-block
   * seed.  The loop-edge allocator marks only conflict-free, call-free chains
   * whose incoming edges materialize one physical register.  This path stays
   * disabled unless NYTRIX_GPR_LOOP_CARRY=1 opts into the experimental carry
   * seeding path; the object encoder's edge proof is not yet sound for all
   * loop-carried call values.
   */
  bool loop_carry_seeding;
  unsigned char
      fused_jcc; /* flags-only CMP jcc opcode for the next BR_IF (0=none) */
  /*
   * Callee-save push count (r12-r15/rbx). Frame/vreg homes sit below them.
   */
  unsigned save_mask;
  int cs_slots;
  nyr_mach_regalloc_t fp_regalloc;
  bool *fp_color_seeded;
  size_t fp_colors_len;
  bool fp_fast_path;
  /*
   * Scalar float color ceiling: xmm8+ hosts vector colors when the vector
   * fast path is active, so the float allocator is clamped to xmm0-xmm7.
   */
  size_t fp_color_limit;
  nyr_mach_regalloc_t vec_regalloc;
  bool *vec_color_seeded;
  size_t vec_colors_len;
  bool vec_fast_path;
  /*
   * Constant vregs are rematerialized at reload sites instead of read from
   * spill homes.
   */
  const int64_t *imm_fold;
  const bool *imm_fold_known;
  const nyr_mach_operand_t *addr_fold;
  const bool *addr_fold_known;
  /*
   * Type-sized, disjoint stack homes for vregs (negative rbp offsets).
   */
  int *vreg_home_offs;
  size_t vreg_home_bytes;
  /*
   * Defining instruction index per vreg (SIZE_MAX when none), built once
   * per function for address-mode decomposition.
   */
  size_t *def_inst;
  /*
   * Code offset of each machine instruction, relative to this function.
   */
  size_t *inst_offsets;
  /*
   * One-time use summaries. Live-out and home-safety checks run from many
   * instruction encoders; rescanning blocks or instructions there made large
   * functions quadratic. These maps preserve the prior conservative checks
   * with O(1) queries and O(vregs) storage.
   */
  bool *vreg_has_ret_use;
  size_t *vreg_last_call_arg_inst;
  bool *vreg_literal_rhs_only;
  /*
   * 0 unknown, 1 no unique carried copy edge, 2 proven edge.
   */
  uint8_t *vreg_carried_copy_cache;
  /*
   * AND(v, 1) ==/!= 0 feeding a single SELECT can be tested at the select.
   */
  int32_t *select_test_src;
  uint8_t *select_test_equal;
  uint8_t *cmp_elision_action;
  uint8_t *select_skip_and;
  int8_t *direct_param_source;
  bool *direct_param_slots;
} nyr_x64_mach_enc_t;

static void mach_free_remat_maps(nyr_x64_mach_enc_t *e) {
  if (!e)
    return;
  free((void *)e->imm_fold);
  free((void *)e->imm_fold_known);
  free((void *)e->addr_fold);
  free((void *)e->addr_fold_known);
  free(e->vreg_home_offs);
  e->imm_fold = NULL;
  e->imm_fold_known = NULL;
  e->addr_fold = NULL;
  e->addr_fold_known = NULL;
  e->vreg_home_offs = NULL;
  e->vreg_home_bytes = 0;
  free(e->inst_offsets);
  e->inst_offsets = NULL;
  free(e->vreg_has_ret_use);
  free(e->vreg_last_call_arg_inst);
  free(e->vreg_literal_rhs_only);
  free(e->vreg_carried_copy_cache);
  free(e->loop_fpr);
  free(e->loop_fpr_entry);
  e->loop_fpr = NULL;
  e->loop_fpr_entry = NULL;
  free(e->loop_gpr);
  e->loop_gpr = NULL;
  free(e->select_test_src);
  free(e->select_test_equal);
  free(e->cmp_elision_action);
  free(e->select_skip_and);
  free(e->direct_param_source);
  free(e->direct_param_slots);
  e->vreg_has_ret_use = NULL;
  e->vreg_last_call_arg_inst = NULL;
  e->vreg_literal_rhs_only = NULL;
  e->vreg_carried_copy_cache = NULL;
  e->select_test_src = NULL;
  e->select_test_equal = NULL;
  e->cmp_elision_action = NULL;
  e->select_skip_and = NULL;
  e->direct_param_source = NULL;
  e->direct_param_slots = NULL;
}
/*
 * Constant vregs remain values rather than locations after lowering. Build
 * the fold map only for uniquely-defined vregs: machine PHIs are represented
 * by edge copies into one destination, so propagating a constant from any one
 * edge would incorrectly make the PHI constant on every incoming path.
 * A unique definition is an ordinary SSA value and may safely carry an
 * immediate through type-preserving copies.
 */
static bool mach_build_imm_fold(const nyr_mach_func_t *mach, int64_t *imm_fold,
                                bool *imm_fold_known) {
  if (!mach || !imm_fold || !imm_fold_known)
    return false;
  if (mach->block_len == 0 || mach->inst_len == 0)
    return true;
  size_t *def_count =
      calloc(mach->vreg_len ? mach->vreg_len : 1, sizeof(*def_count));
  uint32_t *copy_head = ny_malloc_array(mach->vreg_len, sizeof(*copy_head));
  uint32_t *copy_next = ny_malloc_array(mach->vreg_len, sizeof(*copy_next));
  uint32_t *work = ny_malloc_array(mach->vreg_len, sizeof(*work));
  if (!def_count || (mach->vreg_len && (!copy_head || !copy_next || !work))) {
    free(def_count);
    free(copy_head);
    free(copy_next);
    free(work);
    return false;
  }
  for (size_t i = 0; i < mach->inst_len; ++i) {
    const nyr_mach_operand_t *dst = &mach->insts[i].dst;
    if (dst->kind == NYR_MACH_OPERAND_VREG && dst->as.reg < mach->vreg_len &&
        def_count[dst->as.reg] < 2)
      ++def_count[dst->as.reg];
  }
  for (size_t v = 0; v < mach->vreg_len; ++v)
    copy_head[v] = copy_next[v] = UINT32_MAX;
  for (size_t i = 0; i < mach->inst_len; ++i) {
    const nyr_mach_inst_t *in = &mach->insts[i];
    if (in->opcode == NYR_MACH_COPY && in->dst.kind == NYR_MACH_OPERAND_VREG &&
        in->dst.as.reg < mach->vreg_len && def_count[in->dst.as.reg] == 1) {
      if (in->src0.kind == NYR_MACH_OPERAND_IMM) {
        imm_fold[in->dst.as.reg] = in->src0.as.imm;
        imm_fold_known[in->dst.as.reg] = true;
      } else if (in->src0.kind == NYR_MACH_OPERAND_VREG &&
                 in->src0.as.reg < mach->vreg_len &&
                 mach->vreg_classes[in->dst.as.reg] ==
                     mach->vreg_classes[in->src0.as.reg] &&
                 mach->vreg_types[in->dst.as.reg] ==
                     mach->vreg_types[in->src0.as.reg]) {
        copy_next[in->dst.as.reg] = copy_head[in->src0.as.reg];
        copy_head[in->src0.as.reg] = in->dst.as.reg;
      }
    }
  }
  size_t work_len = 0;
  for (size_t v = 0; v < mach->vreg_len; ++v)
    if (imm_fold_known[v])
      work[work_len++] = (uint32_t)v;
  for (size_t i = 0; i < work_len; ++i) {
    uint32_t src = work[i];
    for (uint32_t dst = copy_head[src]; dst != UINT32_MAX;
         dst = copy_next[dst]) {
      if (imm_fold_known[dst])
        continue;
      imm_fold[dst] = imm_fold[src];
      imm_fold_known[dst] = true;
      work[work_len++] = dst;
    }
  }
  free(def_count);
  free(copy_head);
  free(copy_next);
  free(work);
  return true;
}

static bool mach_err(nyr_x64_mach_enc_t *e, const char *msg) {
  if (e && e->err && e->err_len)
    snprintf(e->err, e->err_len, "%s", msg ? msg : "mach encode failed");
  return false;
}

static bool mach_bytes(nyr_x64_mach_enc_t *e, const unsigned char *p,
                       size_t n) {
  if (!nyr_obj_emit(&e->code, p, n))
    return mach_err(e, "x64 machine form encode: out of memory");
  return true;
}

static bool mach_u8(nyr_x64_mach_enc_t *e, unsigned v) {
  unsigned char b = (unsigned char)v;
  return mach_bytes(e, &b, 1);
}
static bool mach_rex(nyr_x64_mach_enc_t *e, bool w, bool r, bool x, bool b) {
  unsigned rex = 0x40u | (w ? 0x08u : 0) | (r ? 0x04u : 0) | (x ? 0x02u : 0) |
                 (b ? 0x01u : 0);
  return rex == 0x40u ? true : mach_u8(e, rex);
}

static bool mach_i32(nyr_x64_mach_enc_t *e, int32_t v) {
  unsigned char b[4] = {
      (unsigned char)(v & 0xff), (unsigned char)((uint32_t)v >> 8),
      (unsigned char)((uint32_t)v >> 16), (unsigned char)((uint32_t)v >> 24)};
  return mach_bytes(e, b, 4);
}

static bool mach_i64(nyr_x64_mach_enc_t *e, int64_t v) {
  for (int i = 0; i < 8; ++i)
    if (!mach_u8(e, (unsigned)(((uint64_t)v >> (i * 8)) & 0xff)))
      return false;
  return true;
}

/*
 * Colors 0..4 are callee-saved GPRs. Self-contained SysV segments outside
 * call blocks may also use caller-saved registers. Fixed-clobber barriers
 * split/home values around division and variable shifts; encoder intrinsics
 * that use caller-saved registers flush them explicitly. Values that cross a
 * call boundary are homed before the call.
 */
static const int mach_color_preg[] = {12, 13, 14, 15, 3, 6, 7, 11, 8, 9, 1, 2};
#define MIR_CALLEE_COLOR_N 5
#define MIR_COLOR_N                                                            \
  ((int)(sizeof(mach_color_preg) / sizeof(mach_color_preg[0])))
static size_t mach_gpr_color_count(const nyr_mach_func_t *mach,
                                   const ny_native_target_info_t *target) {
  (void)mach;
  /*
   * Win64 makes rsi/rdi nonvolatile, while the current save mask covers only
   * the first five colors. Keep those caller-ABI-specific colors SysV-only.
   */
  if (target && target->abi == NY_NATIVE_ABI_WIN64)
    return MIR_CALLEE_COLOR_N;
  return MIR_COLOR_N;
}

/*
 * Hot scalar values benefit from a dedicated physical register. Values whose
 * definitions are all simple scalar moves, loads, and arithmetic can keep
 * that register across basic-block edges; each executed definition updates
 * the same physical home. Callee-saved colors may remain live across calls;
 * caller-saved colors require the stricter call-safety proof below. The
 * interference check reserves each register over every block where its value
 * is live.
 */
typedef struct {
  size_t *starts;
  size_t *ends;
  size_t block_len;
} mach_loop_gpr_ranges_t;

static bool mach_loop_gpr_ranges_build(const nyr_x64_mach_enc_t *e,
                                       mach_loop_gpr_ranges_t *ranges) {
  const nyr_mach_func_t *m = e ? e->mach : NULL;
  if (!m || !ranges || !m->block_len || m->vreg_len > SIZE_MAX / m->block_len ||
      m->vreg_len * m->block_len > SIZE_MAX / sizeof(*ranges->starts))
    return false;
  size_t len = m->vreg_len * m->block_len;
  ranges->starts = malloc(len * sizeof(*ranges->starts));
  ranges->ends = calloc(len, sizeof(*ranges->ends));
  ranges->block_len = m->block_len;
  if (!ranges->starts || !ranges->ends)
    return false;
  for (size_t i = 0; i < len; ++i)
    ranges->starts[i] = SIZE_MAX;
  for (size_t v = 0; v < m->vreg_len; ++v) {
    for (size_t b = 0; b < m->block_len; ++b) {
      size_t at = v * m->block_len + b;
      const nyr_mach_block_t *block = &m->blocks[b];
      if (!block->inst_count)
        continue;
      size_t first = block->first_inst;
      size_t last = first + block->inst_count - 1;
      /*
       * Bound each vreg to its first/last touch in this block. Live-in/out
       * extend that bound to the corresponding edge. This keeps interference
       * conservative while avoiding conflicts with unrelated instructions
       * elsewhere in the same block.
       */
      if (nyr_mach_regalloc_live_in(&e->regalloc, b, (uint32_t)v))
        ranges->starts[at] = first;
      if (nyr_mach_regalloc_live_out(&e->regalloc, b, (uint32_t)v))
        ranges->ends[at] = last;
      for (size_t pc = first; pc <= last; ++pc) {
        const nyr_mach_inst_t *in = &m->insts[pc];
        bool touches =
            in->dst.kind == NYR_MACH_OPERAND_VREG && in->dst.as.reg == v;
        const nyr_mach_operand_t *ops[] = {&in->src0, &in->src1, &in->src2};
        for (size_t k = 0; !touches && k < 3; ++k)
          touches =
              ops[k]->kind == NYR_MACH_OPERAND_VREG && ops[k]->as.reg == v;
        for (size_t k = 0; !touches && k < in->args_len; ++k)
          touches = in->args[k].kind == NYR_MACH_OPERAND_VREG &&
                    in->args[k].as.reg == v;
        if (!touches)
          continue;
        if (ranges->starts[at] == SIZE_MAX || pc < ranges->starts[at])
          ranges->starts[at] = pc;
        if (pc > ranges->ends[at])
          ranges->ends[at] = pc;
      }
    }
  }
  return true;
}

static bool mach_loop_gpr_color_available(const nyr_mach_func_t *m,
                                          const mach_loop_gpr_ranges_t *ranges,
                                          const int *colors, uint32_t vreg,
                                          int color) {
  bool has_interval = false;
  for (size_t b = 0; b < ranges->block_len; ++b)
    has_interval |=
        ranges->starts[(size_t)vreg * ranges->block_len + b] != SIZE_MAX;
  if (!has_interval)
    return false;
  for (size_t other = 0; other < m->vreg_len; ++other) {
    if (colors[other] != color)
      continue;
    for (size_t b = 0; b < ranges->block_len; ++b) {
      size_t a = (size_t)vreg * ranges->block_len + b;
      size_t c = other * ranges->block_len + b;
      if (ranges->starts[a] != SIZE_MAX && ranges->starts[c] != SIZE_MAX &&
          ranges->starts[a] <= ranges->ends[c] &&
          ranges->starts[c] <= ranges->ends[a])
        return false;
    }
  }
  return true;
}

static bool mach_loop_gpr_call_safe(const nyr_mach_func_t *m,
                                    const mach_loop_gpr_ranges_t *ranges,
                                    const unsigned *depth, uint32_t vreg,
                                    bool allow_nonloop_calls) {
  if (!m || !ranges || !depth || vreg >= m->vreg_len)
    return false;
  for (size_t b = 0; b < m->block_len; ++b) {
    const nyr_mach_block_t *block = &m->blocks[b];
    size_t at = (size_t)vreg * ranges->block_len + b;
    if (ranges->starts[at] == SIZE_MAX)
      continue;
    for (size_t n = 0; n < block->inst_count; ++n) {
      size_t pc = block->first_inst + n;
      nyr_mach_opcode_t op = m->insts[pc].opcode;
      if ((op == NYR_MACH_CALL || op == NYR_MACH_INTRINSIC ||
           op == NYR_MACH_INLINE_ASM) &&
          ranges->starts[at] <= pc && pc <= ranges->ends[at]) {
        if (allow_nonloop_calls && op == NYR_MACH_CALL && depth[b] == 0)
          continue;
        return false;
      }
    }
  }
  return true;
}

/*
 * R8 is the only caller-saved color used by the scalar x64 emitter outside
 * call setup. Constant div/mod lowering also uses it as scratch. Caller-safe
 * loop homes are limited to this color and ranges that avoid those ops;
 * calls and inline asm are excluded by mach_loop_gpr_call_safe.
 */
static bool mach_loop_gpr_r8_safe(const nyr_mach_func_t *m,
                                  const mach_loop_gpr_ranges_t *ranges,
                                  uint32_t vreg) {
  if (!m || !ranges || vreg >= m->vreg_len)
    return false;
  for (size_t b = 0; b < m->block_len; ++b) {
    size_t at = (size_t)vreg * ranges->block_len + b;
    if (ranges->starts[at] == SIZE_MAX)
      continue;
    const nyr_mach_block_t *block = &m->blocks[b];
    for (size_t n = 0; n < block->inst_count; ++n) {
      size_t pc = block->first_inst + n;
      if (pc < ranges->starts[at] || pc > ranges->ends[at])
        continue;
      nyr_mach_opcode_t op = m->insts[pc].opcode;
      if (op == NYR_MACH_DIV || op == NYR_MACH_MOD)
        return false;
    }
  }
  return true;
}

/*
 * Caller-saved loop registers also need to avoid instructions whose lowering
 * uses fixed registers. The ordinary segment allocator splits at these
 * boundaries; dedicated loop homes span blocks, so check the complete live
 * range explicitly before assigning one.
 */
static bool mach_inst_uses_vreg(const nyr_mach_inst_t *in, uint32_t vreg);

static void mach_loop_gpr_mark_memory_addresses(const nyr_mach_func_t *m,
                                                bool *memory_address) {
  if (!m || !memory_address)
    return;
  for (size_t i = 0; i < m->inst_len; ++i) {
    const nyr_mach_inst_t *in = &m->insts[i];
    const nyr_mach_operand_t *address = NULL;
    if (in->opcode == NYR_MACH_LOAD)
      address = &in->src0;
    else if (in->opcode == NYR_MACH_STORE)
      address = &in->dst;
    if (address && address->kind == NYR_MACH_OPERAND_VREG &&
        address->as.reg < m->vreg_len)
      memory_address[address->as.reg] = true;
  }
  /*
   * Address expressions can be split across multiple LEAs. Mark every input
   * feeding an address-producing LEA so its register home is not mistaken for
   * an ordinary loop value by the dedicated loop-register allocator.
   */
  bool changed;
  do {
    changed = false;
    for (size_t i = 0; i < m->inst_len; ++i) {
      const nyr_mach_inst_t *in = &m->insts[i];
      if (in->opcode != NYR_MACH_LEA || in->dst.kind != NYR_MACH_OPERAND_VREG ||
          in->dst.as.reg >= m->vreg_len || !memory_address[in->dst.as.reg])
        continue;
      const nyr_mach_operand_t *inputs[] = {&in->src0, &in->src1};
      for (size_t j = 0; j < sizeof(inputs) / sizeof(inputs[0]); ++j)
        if (inputs[j]->kind == NYR_MACH_OPERAND_VREG &&
            inputs[j]->as.reg < m->vreg_len &&
            !memory_address[inputs[j]->as.reg]) {
          memory_address[inputs[j]->as.reg] = true;
          changed = true;
        }
    }
  } while (changed);
}

static bool mach_is_v128(const nyr_mach_func_t *m,
                         const nyr_mach_operand_t *op);

static bool mach_loop_gpr_operand_is_vector(const nyr_mach_func_t *m,
                                            const nyr_mach_operand_t *op) {
  if (!op)
    return false;
  if ((op->kind == NYR_MACH_OPERAND_VREG ||
       op->kind == NYR_MACH_OPERAND_PREG) &&
      op->reg_class == NYR_MACH_REGCLASS_VECTOR)
    return true;
  return mach_is_v128(m, op);
}

/*
 * Current vector encoders use only volatile GPR scratch. Callee-saved loop
 * homes survive them; this range proof is still required for caller-saved R8.
 * Calls are checked independently by mach_loop_gpr_call_safe.
 */
static bool mach_loop_gpr_vector_safe(const nyr_mach_func_t *m,
                                      const mach_loop_gpr_ranges_t *ranges,
                                      uint32_t vreg) {
  if (!m || !ranges || vreg >= m->vreg_len)
    return false;
  for (size_t b = 0; b < m->block_len; ++b) {
    size_t at = (size_t)vreg * ranges->block_len + b;
    if (ranges->starts[at] == SIZE_MAX)
      continue;
    const nyr_mach_block_t *block = &m->blocks[b];
    for (size_t n = 0; n < block->inst_count; ++n) {
      size_t pc = block->first_inst + n;
      if (pc < ranges->starts[at] || pc > ranges->ends[at])
        continue;
      const nyr_mach_inst_t *in = &m->insts[pc];
      const nyr_mach_operand_t *ops[] = {&in->dst, &in->src0, &in->src1,
                                         &in->src2};
      for (size_t k = 0; k < sizeof(ops) / sizeof(ops[0]); ++k)
        if (mach_loop_gpr_operand_is_vector(m, ops[k]))
          return false;
      for (size_t k = 0; k < in->args_len; ++k)
        if (mach_loop_gpr_operand_is_vector(m, &in->args[k]))
          return false;
    }
  }
  return true;
}

static void mach_allocate_loop_gprs(nyr_x64_mach_enc_t *e, const char *name) {
  const nyr_mach_func_t *m = e->mach;
  /*
   * Keep an escape hatch for benchmarking and diagnosis. Only values proven
   * loop-carried by the CFG analysis below are assigned dedicated homes.
   */
  const char *enable_loop_gpr = getenv("NYTRIX_ENABLE_LOOP_GPR");
  if (enable_loop_gpr && (strcmp(enable_loop_gpr, "0") == 0 ||
                          strcmp(enable_loop_gpr, "off") == 0 ||
                          strcmp(enable_loop_gpr, "false") == 0))
    return;
  const char *disable_loop_gpr = getenv("NYTRIX_DISABLE_LOOP_GPR");
  if (disable_loop_gpr && strcmp(disable_loop_gpr, "0") != 0 &&
      strcmp(disable_loop_gpr, "off") != 0 &&
      strcmp(disable_loop_gpr, "false") != 0)
    return;
  /*
   * The whole-function home proof is intentionally bounded to smaller CFGs.
   * Larger functions can have enough alternate paths that keeping a vreg in
   * one register without publishing its canonical home is not yet justified
   * by the current edge analysis (notably, call-heavy tree traversals).
   */
  if (!m->vreg_len || m->vreg_len > 640 || m->block_len > 128 ||
      getenv("NYTRIX_FORCE_STACK_GPR"))
    return;
  size_t *score = calloc(m->vreg_len, sizeof(*score));
  unsigned *defs = calloc(m->vreg_len, sizeof(*defs));
  bool *other_def = calloc(m->vreg_len, sizeof(*other_def));
  bool *loop_carried = calloc(m->vreg_len, sizeof(*loop_carried));
  bool *memory_address = calloc(m->vreg_len, sizeof(*memory_address));
  bool *vector_safe = calloc(m->vreg_len, sizeof(*vector_safe));
  unsigned *depth = calloc(m->block_len, sizeof(*depth));
  int *colors = malloc(m->vreg_len * sizeof(*colors));
  int64_t *folded_value = calloc(m->vreg_len, sizeof(*folded_value));
  bool *folded_known = calloc(m->vreg_len, sizeof(*folded_known));
  mach_loop_gpr_ranges_t ranges = {0};
  if (!score || !defs || !other_def || !loop_carried || !memory_address ||
      !vector_safe || !depth || !colors || !folded_value || !folded_known ||
      !mach_build_imm_fold(m, folded_value, folded_known) ||
      !mach_loop_gpr_ranges_build(e, &ranges)) {
    free(score);
    free(defs);
    free(other_def);
    free(loop_carried);
    free(memory_address);
    free(vector_safe);
    free(depth);
    free(colors);
    free(folded_value);
    free(folded_known);
    free(ranges.starts);
    free(ranges.ends);
    return;
  }
  mach_loop_gpr_mark_memory_addresses(m, memory_address);
  for (size_t v = 0; v < m->vreg_len; ++v)
    vector_safe[v] = mach_loop_gpr_vector_safe(m, &ranges, (uint32_t)v);
  for (size_t v = 0; v < m->vreg_len; ++v) {
    colors[v] = -1;
  }
  /*
   * Build actual CFG edges; block-number intervals include unrelated exits.
   */
  __uint128_t succ[128] = {0}, pred[128] = {0}, dom[128] = {0};
  __uint128_t header_loops[128] = {0};
  const __uint128_t valid_blocks = m->block_len == 128
                                       ? ~(__uint128_t)0
                                       : (((__uint128_t)1 << m->block_len) - 1);
  for (size_t b = 0; b < m->block_len; ++b) {
    const nyr_mach_block_t *block = &m->blocks[b];
    bool falls = true;
    for (size_t n = 0; n < block->inst_count; ++n) {
      const nyr_mach_inst_t *in = &m->insts[block->first_inst + n];
      if ((in->opcode == NYR_MACH_BR || in->opcode == NYR_MACH_BR_IF) &&
          in->src1.kind == NYR_MACH_OPERAND_BLOCK &&
          in->src1.as.block_index < m->block_len) {
        succ[b] |= (__uint128_t)1 << in->src1.as.block_index;
        if (in->opcode == NYR_MACH_BR)
          falls = false;
      } else if (in->opcode == NYR_MACH_RET) {
        falls = false;
      }
    }
    if (falls && b + 1 < m->block_len)
      succ[b] |= (__uint128_t)1 << (b + 1);
  }
  for (size_t b = 0; b < m->block_len; ++b)
    for (size_t t = 0; t < m->block_len; ++t)
      if (succ[b] & ((__uint128_t)1 << t))
        pred[t] |= (__uint128_t)1 << b;
  __uint128_t reachable = 1;
  for (;;) {
    __uint128_t next = reachable;
    for (size_t b = 0; b < m->block_len; ++b)
      if (reachable & ((__uint128_t)1 << b))
        next |= succ[b];
    next &= valid_blocks;
    if (next == reachable)
      break;
    reachable = next;
  }
  for (size_t b = 0; b < m->block_len; ++b)
    if (reachable & ((__uint128_t)1 << b))
      dom[b] = b == 0 ? 1 : reachable;
  bool dom_changed;
  do {
    dom_changed = false;
    for (size_t b = 1; b < m->block_len; ++b) {
      __uint128_t bit = (__uint128_t)1 << b;
      if (!(reachable & bit))
        continue;
      __uint128_t incoming = pred[b] & reachable;
      __uint128_t intersection = reachable;
      if (!incoming)
        intersection = 0;
      else
        for (size_t p = 0; p < m->block_len; ++p)
          if (incoming & ((__uint128_t)1 << p))
            intersection &= dom[p];
      __uint128_t value = intersection | bit;
      if (value != dom[b]) {
        dom[b] = value;
        dom_changed = true;
      }
    }
  } while (dom_changed);
  for (size_t latch = 0; latch < m->block_len; ++latch) {
    for (size_t header = 0; header <= latch; ++header) {
      __uint128_t hbit = (__uint128_t)1 << header;
      if (!(succ[latch] & hbit) || !(reachable & hbit) || !(dom[latch] & hbit))
        continue;
      __uint128_t members = hbit | ((__uint128_t)1 << latch);
      size_t work[128], work_len = 1;
      work[0] = latch;
      for (size_t wi = 0; wi < work_len; ++wi) {
        size_t block = work[wi];
        if (block == header)
          continue;
        __uint128_t incoming = pred[block] & reachable & ~members;
        for (size_t p = 0; p < m->block_len; ++p)
          if (incoming & ((__uint128_t)1 << p)) {
            members |= (__uint128_t)1 << p;
            work[work_len++] = p;
          }
      }
      header_loops[header] |= members;
      for (size_t v = 0; v < m->vreg_len; ++v)
        if (nyr_mach_regalloc_live_out(&e->regalloc, latch, (uint32_t)v) &&
            nyr_mach_regalloc_live_in(&e->regalloc, header, (uint32_t)v))
          loop_carried[v] = true;
    }
  }
  for (size_t header = 0; header < m->block_len; ++header)
    for (size_t b = 0; b < m->block_len; ++b)
      if (header_loops[header] & ((__uint128_t)1 << b))
        ++depth[b];
  /*
   * Call-home publication is not yet proven across loop backedges and joins.
   * The per-vreg range proof below excludes values that cross a call, while
   * allowing unrelated call-free loops in the same function to use registers.
   */
  for (size_t b = 0; b < m->block_len; ++b) {
    const nyr_mach_block_t *block = &m->blocks[b];
    size_t weight =
        depth[b] ? (size_t)1 << (depth[b] > 4 ? 12 : depth[b] * 3) : 0;
    for (size_t n = 0; n < block->inst_count; ++n) {
      const nyr_mach_inst_t *in = &m->insts[block->first_inst + n];
      if (in->dst.kind == NYR_MACH_OPERAND_VREG &&
          in->dst.as.reg < m->vreg_len && in->opcode != NYR_MACH_STORE) {
        uint32_t v = in->dst.as.reg;
        ++defs[v];
        bool loop_home_def =
            (in->opcode == NYR_MACH_COPY &&
             (in->src0.kind == NYR_MACH_OPERAND_VREG ||
              in->src0.kind == NYR_MACH_OPERAND_IMM)) ||
            in->opcode == NYR_MACH_LOAD || in->opcode == NYR_MACH_LEA ||
            in->opcode == NYR_MACH_ADD || in->opcode == NYR_MACH_SUB ||
            in->opcode == NYR_MACH_AND || in->opcode == NYR_MACH_OR ||
            in->opcode == NYR_MACH_XOR || in->opcode == NYR_MACH_MUL ||
            in->opcode == NYR_MACH_SELECT;
        other_def[in->dst.as.reg] |= !loop_home_def;
      }
      const nyr_mach_operand_t *ops[] = {&in->src0, &in->src1, &in->src2};
      for (size_t k = 0; k < 3; ++k)
        if (ops[k]->kind == NYR_MACH_OPERAND_VREG &&
            ops[k]->as.reg < m->vreg_len) {
          uint32_t v = ops[k]->as.reg;
          score[v] += weight;
        }
    }
  }
  bool reserved[MIR_COLOR_N] = {false};
  size_t selected = 0;
  for (int color = 0; color < MIR_COLOR_N; ++color) {
    int preg = mach_color_preg[color];
    bool callee_saved = color < MIR_CALLEE_COLOR_N;
    bool r8_caller_saved = color == 8;
    if (!callee_saved && !r8_caller_saved)
      continue;
    if (preg == 11)
      continue;
    for (;;) {
      size_t best = SIZE_MAX, best_score = 0;
      bool best_is_carried = false;
      for (size_t v = 0; v < m->vreg_len; ++v) {
        bool vector_ok = callee_saved || vector_safe[v];
        if (ny_dbg_is_enabled(NY_DBG_DEEP) && defs[v] > 0 &&
            m->vreg_types[v] == NYR_MACH_TYPE_I64 &&
            (loop_carried[v] || score[v] >= 16)) {
          bool simple_defs = !other_def[v];
          bool unassigned = colors[v] < 0;
          bool score_ok = true;
          bool call_safe = mach_loop_gpr_call_safe(m, &ranges, depth,
                                                   (uint32_t)v, callee_saved);
          bool fixed_safe =
              callee_saved || (r8_caller_saved &&
                               mach_loop_gpr_r8_safe(m, &ranges, (uint32_t)v));
          bool is_unfolded = !folded_known[v];
          bool is_memory_address = memory_address[v];
          bool color_available = mach_loop_gpr_color_available(
              m, &ranges, colors, (uint32_t)v, color);
          ny_dbg_enter_context(name);
          ny_dbg_heuristic_event(
              "X64_LOOP_HOME",
              "candidate v%zu preg=%d score=%zu carried=%d eligible=%d defs=%d "
              "simple=%d unassigned=%d unfolded=%d call-safe=%d fixed-safe=%d "
              "vector-safe=%d memory-address=%d available=%d",
              v, mach_color_preg[color], score[v], (int)loop_carried[v],
              (int)(simple_defs && unassigned && score_ok && call_safe &&
                    fixed_safe && vector_ok && is_unfolded && color_available),
              1, (int)simple_defs, (int)unassigned, (int)is_unfolded,
              (int)call_safe, (int)fixed_safe, (int)vector_ok,
              (int)is_memory_address, (int)color_available);
          ny_dbg_exit_context(name);
        }
        bool fixed_safe =
            callee_saved ||
            (r8_caller_saved && mach_loop_gpr_r8_safe(m, &ranges, (uint32_t)v));
        bool candidate_loop_local =
            loop_carried[v] || (!callee_saved && score[v] >= 16);
        /*
         * A dedicated color has the same meaning on every CFG edge. Caller-
         * saved R8 is limited to high-use loop-local values with no call or
         * division barrier in their live range; share colors only between
         * disjoint ranges and reserve them from the ordinary allocator.
         */
        if (defs[v] > 0 && candidate_loop_local && !other_def[v] &&
            colors[v] < 0 && m->vreg_types[v] == NYR_MACH_TYPE_I64 &&
            !folded_known[v] &&
            mach_loop_gpr_call_safe(m, &ranges, depth, (uint32_t)v,
                                    callee_saved) &&
            fixed_safe && vector_ok &&
            (best == SIZE_MAX ||
             (loop_carried[v] != best_is_carried ? loop_carried[v]
                                                 : score[v] > best_score)) &&
            mach_loop_gpr_color_available(m, &ranges, colors, (uint32_t)v,
                                          color)) {
          best = v;
          best_score = score[v];
          best_is_carried = loop_carried[v];
        }
      }
      if (best == SIZE_MAX)
        break;
      colors[best] = color;
      reserved[color] = true;
      ++selected;
      if (ny_dbg_is_enabled(NY_DBG_DEEP)) {
        ny_dbg_enter_context(name);
        ny_dbg_heuristic_event(
            "X64_LOOP_HOME", "v%zu preg=%d weighted-uses=%zu carried=%d", best,
            mach_color_preg[color], best_score, (int)best_is_carried);
        ny_dbg_exit_context(name);
      }
    }
  }
  if (selected) {
    e->loop_gpr = colors;
    colors = NULL;
    /* Spill ordinary values occupying colors reserved for loop homes. */
    for (size_t i = 0; i < e->regalloc.segment_len; ++i) {
      nyr_mach_live_segment_t *s = &e->regalloc.segments[i];
      if (s->vreg >= m->vreg_len || e->loop_gpr[s->vreg] >= 0 || s->color < 0 ||
          s->color >= MIR_COLOR_N || !reserved[s->color])
        continue;
      s->color = -1;
      s->reload = true;
      s->spill = true;
      s->carried = s->acyclic_carried = false;
      if (s->spill_reason != NYR_MACH_SPILL_FIXED_CLOBBER)
        s->spill_reason = NYR_MACH_SPILL_REGISTER_PRESSURE;
    }
    for (size_t i = 0; i < e->regalloc.segment_len; ++i) {
      nyr_mach_live_segment_t *s = &e->regalloc.segments[i];
      int c = e->loop_gpr[s->vreg];
      if (c < 0)
        continue;
      s->color = c;
      s->carried = s->acyclic_carried = false;
      s->reload = s->spill = true;
    }
  }
  free(score);
  free(defs);
  free(other_def);
  free(loop_carried);
  free(memory_address);
  free(vector_safe);
  free(depth);
  free(colors);
  free(folded_value);
  free(folded_known);
  free(ranges.starts);
  free(ranges.ends);
}
/*
 * Stack layout below %rbp (after push %rbp; mov %rsp,%rbp):
 *   [rbp - 8*cs_slots .. rbp)     saved colors used by this function
 *   [next .. )                    frame slots [0..frame_slot_len)
 *   [after frame .. )             vreg spill homes
 * Vreg homes are packed by machine type: scalar/FPR values reserve 8 bytes,
 * V128 values 16, and V256 values 32. Only scalar GPR homes with one
 * verified, straight-line live segment share an offset; frame slots,
 * including address-taken locals, remain disjoint. Vector loads/stores use
 * unaligned-safe forms, so 8-byte packing alignment is sufficient.
 */
static int mach_cs_count(unsigned mask) {
  int n = 0;
  for (int i = 0; i < MIR_CALLEE_COLOR_N; ++i)
    if (mask & (1u << i))
      ++n;
  return n;
}

static int mach_vreg_home_off(const nyr_x64_mach_enc_t *e, uint32_t vreg) {
  if (!e || !e->mach || !e->vreg_home_offs || vreg >= e->mach->vreg_len)
    return 0;
  return e->vreg_home_offs[vreg];
}

static size_t mach_vreg_home_size(const nyr_mach_func_t *mach, size_t vreg) {
  if (!mach || !mach->vreg_types || vreg >= mach->vreg_len)
    return 8;
  switch (mach->vreg_types[vreg]) {
  case NYR_MACH_TYPE_V256_I64:
  case NYR_MACH_TYPE_V256_F64:
    return 32;
  case NYR_MACH_TYPE_V128_I64:
  case NYR_MACH_TYPE_V128_F64:
  case NYR_MACH_TYPE_V128_F32:
    return 16;
  default:
    return 8;
  }
}

typedef struct {
  size_t start;
  size_t end;
  uint32_t vreg;
  uint32_t block;
} mach_home_interval_t;

static int mach_home_interval_cmp(const void *lhs, const void *rhs) {
  const mach_home_interval_t *a = lhs;
  const mach_home_interval_t *b = rhs;
  if (a->start != b->start)
    return a->start < b->start ? -1 : 1;
  if (a->end != b->end)
    return a->end < b->end ? -1 : 1;
  return a->vreg < b->vreg ? -1 : a->vreg != b->vreg;
}

static bool mach_home_color_boundary(const nyr_mach_inst_t *in) {
  if (!in)
    return true;
  if (in->opcode == NYR_MACH_CALL || in->opcode == NYR_MACH_DIV ||
      in->opcode == NYR_MACH_MOD || in->opcode == NYR_MACH_INTRINSIC ||
      ((in->opcode == NYR_MACH_SHL || in->opcode == NYR_MACH_SAR ||
        in->opcode == NYR_MACH_ROR || in->opcode == NYR_MACH_ROR32) &&
       in->src1.kind != NYR_MACH_OPERAND_IMM))
    return true;
  return in->dst.kind == NYR_MACH_OPERAND_PREG ||
         in->src0.kind == NYR_MACH_OPERAND_PREG ||
         in->src1.kind == NYR_MACH_OPERAND_PREG ||
         in->src2.kind == NYR_MACH_OPERAND_PREG;
}

static bool mach_vreg_home_colorable(const nyr_x64_mach_enc_t *e, size_t vreg,
                                     mach_home_interval_t *out) {
  if (!e || !e->mach || !out || !e->mach->vreg_types ||
      !e->mach->vreg_classes || vreg >= e->mach->vreg_len ||
      e->mach->vreg_classes[vreg] != NYR_MACH_REGCLASS_GPR ||
      (e->mach->vreg_types[vreg] != NYR_MACH_TYPE_I64 &&
       e->mach->vreg_types[vreg] != NYR_MACH_TYPE_PTR) ||
      !e->regalloc.vreg_offsets || e->regalloc.vreg_len != e->mach->vreg_len)
    return false;
  size_t first = e->regalloc.vreg_offsets[vreg];
  size_t last = e->regalloc.vreg_offsets[vreg + 1];
  if (last != first + 1 || last > e->regalloc.segment_len)
    return false;
  const nyr_mach_live_segment_t *seg = &e->regalloc.segments[first];
  if (seg->vreg != vreg || seg->reload || seg->spill || seg->carried ||
      seg->block >= e->mach->block_len || seg->start > seg->end ||
      seg->end >= e->mach->inst_len)
    return false;
  const nyr_mach_block_t *block = &e->mach->blocks[seg->block];
  if (!block->inst_count || seg->start < block->first_inst ||
      seg->end >= block->first_inst + block->inst_count)
    return false;
  for (size_t pc = seg->start; pc <= seg->end; ++pc) {
    const nyr_mach_inst_t *in = &e->mach->insts[pc];
    if (mach_home_color_boundary(in))
      return false;
    /*
     * A frame-loaded value's home identifies its incoming argument home.
     * Never share it with an unrelated interval.
     */
    if (in->opcode == NYR_MACH_LOAD && in->dst.kind == NYR_MACH_OPERAND_VREG &&
        in->dst.as.reg == vreg && in->src0.kind == NYR_MACH_OPERAND_FRAME)
      return false;
  }
  *out = (mach_home_interval_t){.start = seg->start,
                                .end = seg->end,
                                .vreg = (uint32_t)vreg,
                                .block = seg->block};
  return true;
}

static bool mach_home_end_before(const size_t *home_ends, size_t lhs,
                                 size_t rhs) {
  return home_ends[lhs] < home_ends[rhs] ||
         (home_ends[lhs] == home_ends[rhs] && lhs < rhs);
}

static void mach_home_active_push(size_t *active, size_t *active_len,
                                  const size_t *home_ends, size_t home) {
  size_t at = (*active_len)++;
  while (at > 0) {
    size_t parent = (at - 1u) / 2u;
    if (!mach_home_end_before(home_ends, home, active[parent]))
      break;
    active[at] = active[parent];
    at = parent;
  }
  active[at] = home;
}

static size_t mach_home_active_pop(size_t *active, size_t *active_len,
                                   const size_t *home_ends) {
  size_t result = active[0];
  size_t tail = active[--(*active_len)];
  if (*active_len == 0)
    return result;
  size_t at = 0;
  while (at < *active_len / 2u) {
    size_t child = at * 2u + 1u;
    if (child + 1u < *active_len &&
        mach_home_end_before(home_ends, active[child + 1u], active[child]))
      ++child;
    if (!mach_home_end_before(home_ends, active[child], tail))
      break;
    active[at] = active[child];
    at = child;
  }
  active[at] = tail;
  return result;
}

static bool mach_append_vreg_home(nyr_x64_mach_enc_t *e,
                                  size_t frame_slots_bytes, size_t width,
                                  size_t *used, int *out) {
  if (!e || !used || !out || *used > SIZE_MAX - width ||
      frame_slots_bytes > SIZE_MAX - *used - width)
    return false;
  *used += width;
  size_t below_rbp = (size_t)e->cs_slots * 8u + frame_slots_bytes + *used;
  if (below_rbp > (size_t)INT_MAX)
    return false;
  *out = -(int)below_rbp;
  return true;
}

static bool mach_build_vreg_homes(nyr_x64_mach_enc_t *e,
                                  size_t frame_slots_bytes) {
  if (!e || !e->mach)
    return false;
  const nyr_mach_func_t *mach = e->mach;
  if (!mach->vreg_len) {
    e->vreg_home_bytes = 0;
    return true;
  }
  int *offs = calloc(mach->vreg_len, sizeof(*offs));
  mach_home_interval_t *intervals = calloc(mach->vreg_len, sizeof(*intervals));
  size_t *home_ends = calloc(mach->vreg_len, sizeof(*home_ends));
  int *home_offs = calloc(mach->vreg_len, sizeof(*home_offs));
  if (!offs || !intervals || !home_ends || !home_offs) {
    free(offs);
    free(intervals);
    free(home_ends);
    free(home_offs);
    return mach_err(e, "x64 machine form encode: OOM (vreg homes)");
  }
  size_t interval_len = 0;
  for (size_t v = 0; v < mach->vreg_len; ++v)
    if (mach_vreg_home_colorable(e, v, &intervals[interval_len]))
      ++interval_len;
  qsort(intervals, interval_len, sizeof(*intervals), mach_home_interval_cmp);
  size_t *active_homes = calloc(mach->vreg_len, sizeof(*active_homes));
  size_t *free_homes = calloc(mach->vreg_len, sizeof(*free_homes));
  if (!active_homes || !free_homes) {
    free(active_homes);
    free(free_homes);
    free(offs);
    free(intervals);
    free(home_ends);
    free(home_offs);
    return mach_err(e, "x64 machine form encode: OOM (home interval map)");
  }
  size_t used = 0, home_len = 0, active_len = 0, free_len = 0;
  for (size_t i = 0; i < interval_len; ++i) {
    const mach_home_interval_t *interval = &intervals[i];
    /*
     * These intervals have exactly one non-spilled segment and no uses
     * outside their defining block. Their homes can therefore share across
     * blocks as well as within one block: a block-local value is dead when
     * control leaves its block.
     */
    while (active_len && home_ends[active_homes[0]] < interval->start)
      free_homes[free_len++] =
          mach_home_active_pop(active_homes, &active_len, home_ends);
    size_t home = free_len ? free_homes[--free_len] : SIZE_MAX;
    if (home == SIZE_MAX) {
      home = home_len;
      if (!mach_append_vreg_home(e, frame_slots_bytes, 8, &used,
                                 &home_offs[home])) {
        free(active_homes);
        free(free_homes);
        free(offs);
        free(intervals);
        free(home_ends);
        free(home_offs);
        return mach_err(e, "x64 machine form encode: vreg home overflow");
      }
      ++home_len;
    }
    offs[interval->vreg] = home_offs[home];
    home_ends[home] = interval->end;
    mach_home_active_push(active_homes, &active_len, home_ends, home);
  }
  size_t colored_homes = home_len;
  for (size_t v = 0; v < mach->vreg_len; ++v) {
    if (offs[v])
      continue;
    if (!mach_append_vreg_home(e, frame_slots_bytes,
                               mach_vreg_home_size(mach, v), &used, &offs[v])) {
      free(offs);
      free(intervals);
      free(home_ends);
      free(home_offs);
      free(active_homes);
      free(free_homes);
      return mach_err(e, "x64 machine form encode: vreg home overflow");
    }
  }
  ny_dbg_heuristic_event(
      "regalloc",
      "home-layout vregs=%zu single-block=%zu unique=%zu reused=%zu "
      "bytes=%zu",
      mach->vreg_len, interval_len, colored_homes,
      interval_len > colored_homes ? interval_len - colored_homes : 0, used);
  free(intervals);
  free(home_ends);
  free(home_offs);
  free(active_homes);
  free(free_homes);
  e->vreg_home_offs = offs;
  e->vreg_home_bytes = used;
  return true;
}
static int mach_slot_off(const nyr_x64_mach_enc_t *e,
                         const nyr_mach_operand_t *op) {
  if (!e || !e->mach || !op)
    return 0;
  if (op->kind == NYR_MACH_OPERAND_FRAME) {
    /*
     * Walk per-slot sizes so vector locals (V128=16B, V256=32B) get
     * non-overlapping offsets instead of a fixed 8-byte stride.
     */
    size_t off = 0;
    for (size_t slot = 0;
         slot < op->as.frame_index && slot < e->mach->frame_slot_len; ++slot) {
      size_t ssz = e->mach->frame_slots[slot].size;
      if (ssz < 8)
        ssz = 8;
      size_t salign = e->mach->frame_slots[slot].align;
      if (salign < 1)
        salign = 1;
      off = (off + salign - 1) & ~(salign - 1);
      off += ssz;
    }
    return -((int)(e->cs_slots * 8u + off + 8));
  }
  if (op->kind == NYR_MACH_OPERAND_VREG)
    return mach_vreg_home_off(e, op->as.reg);
  return 0;
}

static void mach_rax_invalidate(nyr_x64_mach_enc_t *e);
static bool mach_is_i64(const nyr_mach_func_t *m, const nyr_mach_operand_t *op);
static bool mach_op_is_vreg(const nyr_mach_operand_t *op, uint32_t vreg);
static bool mach_vreg_future_use_requires_home(const nyr_x64_mach_enc_t *e,
                                               uint32_t vreg);
static bool mach_vreg_used_in_later_block(const nyr_x64_mach_enc_t *e,
                                          uint32_t vreg);
static bool mach_vreg_has_ret_use(const nyr_x64_mach_enc_t *e, uint32_t vreg);
static bool mach_vreg_can_feed_from_color(const nyr_x64_mach_enc_t *e,
                                          uint32_t vreg);
static bool
mach_vreg_same_block_future_use_requires_home(const nyr_x64_mach_enc_t *e,
                                              uint32_t vreg);
static bool mach_load_call_arg_rax(nyr_x64_mach_enc_t *e,
                                   const nyr_mach_operand_t *arg);
static bool mach_load_call_arg_preg(nyr_x64_mach_enc_t *e,
                                    const nyr_mach_operand_t *arg,
                                    unsigned preg);
static bool mach_load_rax(nyr_x64_mach_enc_t *e, int off);
static bool mach_load_op_rax(nyr_x64_mach_enc_t *e,
                             const nyr_mach_operand_t *op);
static bool mach_load_op_preg(nyr_x64_mach_enc_t *e,
                              const nyr_mach_operand_t *op, unsigned preg);
static bool mach_store_preg_home(nyr_x64_mach_enc_t *e, int preg, int off);
static bool mach_vreg_can_forward_to_next_rax(const nyr_x64_mach_enc_t *e,
                                              uint32_t vreg);
static void mach_r10_index_invalidate(nyr_x64_mach_enc_t *e);

#define MFP_COLOR_N 14
/*
 * Vector colors live in the upper XMM half so scalar-float and vector values
 * can be resident in one function without overlapping: xmm8..xmm13 hold
 * vector colors, xmm14/xmm15 stay dedicated vector scratch, and the scalar
 * float allocator is clamped to xmm0..xmm7 whenever the vector fast path is
 * active (see the fp_color_limit wiring in mach_encode_function).
 */
#define MVEC_COLOR_N 6
#define MVEC_PREG_BASE 8
#define MACH_SEEDED_STACK_MAX 64

static int mach_vreg_fpreg(const nyr_x64_mach_enc_t *e, uint32_t vreg) {
  if (e && e->loop_fpr && vreg < e->fp_colors_len && e->loop_fpr[vreg] >= 0)
    return e->loop_fpr[vreg];
  if (!e || !e->fp_fast_path || vreg >= e->fp_colors_len)
    return -1;
  const nyr_mach_live_segment_t *seg =
      nyr_mach_regalloc_segment_at(&e->fp_regalloc, vreg, e->cur_inst);
  if (!seg || seg->color < 0 || (size_t)seg->color >= e->fp_color_limit)
    return -1;
  return seg->color;
}

static int mach_vreg_vpreg(const nyr_x64_mach_enc_t *e, uint32_t vreg) {
  if (!e || !e->vec_fast_path || vreg >= e->vec_colors_len)
    return -1;
  const nyr_mach_live_segment_t *seg =
      nyr_mach_regalloc_segment_at(&e->vec_regalloc, vreg, e->cur_inst);
  if (!seg || seg->color < 0 || seg->color >= MVEC_COLOR_N)
    return -1;
  return MVEC_PREG_BASE + seg->color;
}

static int mach_vreg_assigned_preg(const nyr_x64_mach_enc_t *e, uint32_t vreg) {
  if (e && e->loop_gpr && vreg < e->colors_len && e->loop_gpr[vreg] >= 0)
    return mach_color_preg[e->loop_gpr[vreg]];
  if (!e || vreg >= e->colors_len || e->force_stack_reads)
    return -1;
  const nyr_mach_live_segment_t *seg =
      nyr_mach_regalloc_segment_at(&e->regalloc, vreg, e->cur_inst);
  if (!seg || seg->color < 0 || seg->color >= MIR_COLOR_N)
    return -1;
  return mach_color_preg[seg->color];
}

static const nyr_mach_live_segment_t *
mach_fsegment_at(const nyr_x64_mach_enc_t *e, uint32_t vreg, size_t inst) {
  return e ? nyr_mach_regalloc_segment_at(&e->fp_regalloc, vreg, inst) : NULL;
}

static bool mach_fsegment_carries(const nyr_x64_mach_enc_t *e, uint32_t vreg,
                                  const nyr_mach_live_segment_t *seg) {
  if (!e || !seg || seg->start == 0 || seg->reload || seg->color < 0)
    return false;
  const nyr_mach_live_segment_t *prev =
      mach_fsegment_at(e, vreg, seg->start - 1);
  return prev && prev->end + 1 == seg->start && !prev->spill &&
         prev->color == seg->color;
}

static int mach_vreg_preg(const nyr_x64_mach_enc_t *e, uint32_t vreg) {
  int preg = mach_vreg_assigned_preg(e, vreg);
  if (preg < 0)
    return -1;
  /*
   * The assigned preg is only a usable value after this segment seeded it:
   * at block entry via a proven carried edge, at its definition, or by
   * mach_seed_colored_preg loading the canonical home. Calls clear the seed
   * after publishing homes; accepting an unseeded same-block value here
   * would reuse the caller-clobbered preg instead of reloading its value.
   */
  if (!e->color_seeded || !e->color_seeded[vreg])
    return -1;
  return preg;
}

static const nyr_mach_live_segment_t *
mach_segment_at(const nyr_x64_mach_enc_t *e, uint32_t vreg, size_t inst) {
  return e ? nyr_mach_regalloc_segment_at(&e->regalloc, vreg, inst) : NULL;
}

/*
 * Producer-side color: the preg the allocator reserved for vreg's segment
 * at the current instruction. Unlike mach_vreg_preg this is not gated by
 * the carried-copy proof — a def must publish into its colored register so
 * a carry chain the allocator proved stays materialized, while the
 * canonical home (written per requires_home_after_def) serves every other
 * consumer.
 */
static int mach_vreg_def_preg(const nyr_x64_mach_enc_t *e, uint32_t vreg) {
  return mach_vreg_assigned_preg(e, vreg);
}

static bool mach_segment_carries(const nyr_x64_mach_enc_t *e, uint32_t vreg,
                                 const nyr_mach_live_segment_t *seg) {
  if (!e || !seg || seg->start == 0 || seg->reload || seg->color < 0)
    return false;
  const nyr_mach_live_segment_t *prev =
      mach_segment_at(e, vreg, seg->start - 1);
  return prev && prev->end + 1 == seg->start && !prev->spill &&
         prev->color == seg->color;
}

/*
 * Map machine-form comparison condition to x86 setcc opcode.
 * Integer path uses signed setl/setle/setg/setge; float path uses
 * unordered-compatible setb/setbe/seta/setae after ucomisd.
 */
static unsigned char mach_cond_setcc(nyr_mach_cond_t cond, bool is_float) {
  static const unsigned char int_tab[] = {
      0x94, /* EQ  -> sete  */
      0x95, /* NE  -> setne */
      0x9C, /* LT  -> setl  */
      0x9E, /* LE  -> setle */
      0x9F, /* GT  -> setg  */
      0x9D, /* GE  -> setge */
  };
  static const unsigned char float_tab[] = {
      0x94, /* EQ  -> sete  (ZF=1)         */
      0x95, /* NE  -> setne (ZF=0)         */
      0x92, /* LT  -> setb  (CF=1, a<b)    */
      0x96, /* LE  -> setbe (CF=1 or ZF=1) */
      0x97, /* GT  -> seta  (CF=0, ZF=0)   */
      0x93, /* GE  -> setae (CF=0)         */
  };
  const unsigned char *tab = is_float ? float_tab : int_tab;
  unsigned idx = (unsigned)cond - (unsigned)NYR_MACH_COND_EQ;
  if (idx >= 6)
    return 0;
  return tab[idx];
}

/*
 * Map machine-form condition to x86 jcc rel32 opcode (0F 8x).  The float
 * table uses the unordered-compatible unsigned compares that mirror the
 * float setcc table after ucomis[sd].
 */
static unsigned char mach_cond_jcc(nyr_mach_cond_t cond, bool is_float) {
  static const unsigned char int_tab[] = {
      0x84, /* EQ  -> je  */
      0x85, /* NE  -> jne */
      0x8C, /* LT  -> jl  */
      0x8E, /* LE  -> jle */
      0x8F, /* GT  -> jg  */
      0x8D, /* GE  -> jge */
  };
  static const unsigned char float_tab[] = {
      0x84, /* EQ  -> je  */
      0x85, /* NE  -> jne */
      0x82, /* LT  -> jb  */
      0x86, /* LE  -> jbe */
      0x87, /* GT  -> ja  */
      0x83, /* GE  -> jae */
  };
  const unsigned char *tab = is_float ? float_tab : int_tab;
  unsigned idx = (unsigned)cond - (unsigned)NYR_MACH_COND_EQ;
  if (idx >= 6)
    return 0;
  return tab[idx];
}

static bool mach_begin_segments(nyr_x64_mach_enc_t *e) {
  if (!e || !e->color_seeded)
    return true;
  for (uint32_t v = 0; v < e->colors_len; ++v) {
    /*
     * Loop-resident values keep a dedicated callee-saved home register for
     * the whole function (see mach_allocate_loop_gprs): every definition
     * commits into that preg, no fixed-register sequence can clobber it,
     * and the register is reserved function-wide. The block-entry seed set
     * for live-in loop homes therefore survives across the edge; clearing it
     * here would force a home reload even though the arriving register still
     * holds the current value. Values chosen for loop homes never cross
     * calls and are never address-taken, so no join can observe a stale
     * register.
     */
    if (e->loop_gpr && e->loop_gpr[v] >= 0)
      continue;
    /*
     * The block-entry seed below is a conjunction that includes the current
     * seed state, so an unseeded value can only stay unseeded. Every term
     * that feeds the assignment is a pure read (the segment lookup and the
     * carry proof never mutate the allocator), so the whole update is a
     * no-op for an unseeded value. Skip the segment lookups in that case:
     * they are the per-value, per-instruction cost of this pass, and a
     * block that never seeded a value never needs to re-prove it.
     */
    if (!e->color_seeded[v])
      continue;
    const nyr_mach_live_segment_t *seg = mach_segment_at(e, v, e->cur_inst);
    if (!seg || seg->start != e->cur_inst)
      continue;
    /*
     * A carried segment arrives in its preg with no reload: the regalloc
     * pass coalesced the phi-edge copy chain to one color, so the value
     * survived the loop backedge without flushing to its stack home.
     */
    bool carried = seg->carried || mach_segment_carries(e, v, seg);
    /*
     * The allocator marks a chunk carried when the previous chunk is
     * contiguous and same-color, but the encoder only loads a reload chunk
     * on first USE.  A use-free reload chunk (created by live-range
     * splitting) never materializes its preg, so claiming carry here would
     * leave the stale value resident.  Gate the carry on the preg actually
     * being proven resident in this block, and on that preg being the one
     * the current segment occupies: a split can recolor the value, and a
     * reload segment must always re-materialize from the canonical home.
     */
    int current_preg = -1;
    if (seg->color >= 0 && seg->color < MIR_COLOR_N)
      current_preg = mach_color_preg[seg->color];
    /*
     * Carry proof is local to this segment. A vreg can be carried through
     * separate CFG regions with different colors; consulting any carried
     * segment for the vreg can incorrectly seed this segment from an
     * unrelated register. The block-entry seed is established only for a
     * carried segment whose incoming edges share this preg, while an
     * intra-block split is covered by mach_segment_carries().
     */
    e->color_seeded[v] =
        !seg->reload && current_preg >= 0 && e->color_seeded[v] && carried;
  }
  return true;
}

static bool mach_block_unconditional_self_loop(const nyr_x64_mach_enc_t *e,
                                               size_t block) {
  if (!e || !e->mach || block >= e->mach->block_len)
    return false;
  const nyr_mach_block_t *b = &e->mach->blocks[block];
  if (!b->inst_count)
    return false;
  const nyr_mach_inst_t *last =
      &e->mach->insts[b->first_inst + b->inst_count - 1];
  return last->opcode == NYR_MACH_BR &&
         last->src1.kind == NYR_MACH_OPERAND_BLOCK &&
         last->src1.as.block_index == block;
}

static size_t mach_block_for_inst(const nyr_x64_mach_enc_t *e, size_t inst) {
  if (!e || !e->mach)
    return SIZE_MAX;
  /*
   * The encoder walks one block at a time. Most queries happen while that
   * block is active, so avoid rescanning the CFG for every vreg/home query.
   */
  if (e->cur_block_valid && e->cur_block < e->mach->block_len) {
    const nyr_mach_block_t *b = &e->mach->blocks[e->cur_block];
    if (inst >= b->first_inst && inst - b->first_inst < b->inst_count)
      return e->cur_block;
  }
  for (size_t block = 0; block < e->mach->block_len; ++block) {
    const nyr_mach_block_t *b = &e->mach->blocks[block];
    if (inst >= b->first_inst && inst - b->first_inst < b->inst_count)
      return block;
  }
  return SIZE_MAX;
}

static bool mach_end_segments(nyr_x64_mach_enc_t *e) {
  if (!e || !e->color_seeded)
    return true;
  for (uint32_t v = 0; v < e->colors_len; ++v) {
    if (e->loop_gpr && e->loop_gpr[v] >= 0)
      continue;
    if (!e->color_seeded[v])
      continue;
    const nyr_mach_live_segment_t *seg = mach_segment_at(e, v, e->cur_inst);
    if (!seg || seg->end != e->cur_inst)
      continue;
    const nyr_mach_live_segment_t *next =
        mach_segment_at(e, v, e->cur_inst + 1);
    size_t block = mach_block_for_inst(e, e->cur_inst);
    bool preserve_self_loop =
        block != SIZE_MAX && mach_block_unconditional_self_loop(e, block) &&
        nyr_mach_regalloc_live_in(&e->regalloc, block, v) && !seg->spill;
    /*
     * A carried value continues in its preg across the edge: no home store
     * needed unless it spills (live-out headers still home for the exit
     * path, which reloads from the canonical stack slot).
     */
    bool needs_home = !preserve_self_loop &&
                      (seg->spill || (!seg->carried && next &&
                                      !mach_segment_carries(e, v, next)));
    if (mach_vreg_has_ret_use(e, v))
      needs_home = false;
    /*
     * A later-block use normally re-seeds from a home kept current at the
     * def, so the block-end store is redundant.  When the allocator marked
     * this carried segment spill=true it did so precisely because a
     * consumer outside the carried chain reads the home (a loop-exit edge):
     * the def itself skipped its home write (the chain is carried), so this
     * store is the only thing keeping that home current.
     */
    if (mach_vreg_used_in_later_block(e, v) && !seg->spill)
      needs_home = false;
    if (needs_home && seg->color >= 0 && e->color_seeded[v] &&
        !e->force_stack_reads) {
      int preg = mach_color_preg[seg->color];
      int off = mach_vreg_home_off(e, v);
      if (!mach_store_preg_home(e, preg, off))
        return false;
      mach_rax_invalidate(e);
    }
    if (seg->spill || (!preserve_self_loop && !seg->carried &&
                       !mach_segment_at(e, v, e->cur_inst + 1)))
      e->color_seeded[v] = false;
  }
  return true;
}

static bool mach_add_reloc(nyr_x64_mach_enc_t *e, const char *sym, int type);

static bool mach_remat_addr_preg(nyr_x64_mach_enc_t *e, int preg,
                                 const nyr_mach_operand_t *addr) {
  if (!e || !addr || preg < 0)
    return false;
  if (addr->kind == NYR_MACH_OPERAND_FRAME) {
    unsigned char modrm = (unsigned char)(0x85 | ((preg & 7) << 3));
    return mach_rex(e, true, preg >= 8, false, false) && mach_u8(e, 0x8d) &&
           mach_u8(e, modrm) && mach_i32(e, mach_slot_off(e, addr));
  }
  if (addr->kind == NYR_MACH_OPERAND_SYMBOL && addr->as.symbol) {
    unsigned char modrm = (unsigned char)(0x05 | ((preg & 7) << 3));
    return mach_rex(e, true, preg >= 8, false, false) && mach_u8(e, 0x8d) &&
           mach_u8(e, modrm) &&
           mach_add_reloc(e, addr->as.symbol, NY_RELOC_PC32) && mach_i32(e, 0);
  }
  return false;
}

/*
 * If preg not yet proven in this block, reload from dual-written stack home.
 */
static bool mach_seed_colored_preg(nyr_x64_mach_enc_t *e, uint32_t vreg) {
  if (!e || vreg >= e->colors_len)
    return true;
  int preg = mach_vreg_assigned_preg(e, vreg);
  if (preg < 0)
    return true;
  if (e->color_seeded && e->color_seeded[vreg])
    return true;
  int off = mach_vreg_home_off(e, vreg);
  if (e->imm_fold && e->imm_fold_known && e->imm_fold_known[vreg]) {
    /*
     * movabsq $imm64, %preg: rematerialize cheap constants at reload sites.
     */
    int64_t imm = e->imm_fold[vreg];
    if (imm == 0) {
      /*
       * xor r32,r32: zero idiom, shortest encoding.
       */
      if ((preg >= 8 && !mach_u8(e, 0x45)) || !mach_u8(e, 0x31) ||
          !mach_u8(e, (unsigned)(0xc0 | ((preg & 7) << 3) | (preg & 7))))
        return false;
    } else if ((uint64_t)imm <= 0xffffffffu) {
      /*
       * mov r32,imm32 zero-extends to 64 bits; up to 4B shorter.
       */
      if ((preg >= 8 && !mach_u8(e, 0x41)) ||
          !mach_u8(e, (unsigned)(0xb8 + (preg & 7))) ||
          !mach_i32(e, (int32_t)(uint32_t)imm))
        return false;
    } else if (!mach_rex(e, true, false, false, preg >= 8) ||
               !mach_u8(e, (unsigned)(0xb8 + (preg & 7))) || !mach_i64(e, imm))
      return false;
  } else if (e->addr_fold && e->addr_fold_known && e->addr_fold_known[vreg]) {
    /*
     * Recreate frame/symbol addresses with LEA instead of reading a spill.
     */
    if (!mach_remat_addr_preg(e, preg, &e->addr_fold[vreg]))
      return false;
  } else {
    /*
     * movq off(%rbp), %preg — 8B /r dest=REG(preg), src=disp32(rbp).
     */
    unsigned char modrm = (unsigned char)(0x85 | ((preg & 7) << 3));
    if (!mach_rex(e, true, preg >= 8, false, false) || !mach_u8(e, 0x8b) ||
        !mach_u8(e, modrm) || !mach_i32(e, off))
      return false;
  }
  if (e->color_seeded)
    e->color_seeded[vreg] = true;
  mach_rax_invalidate(e);
  return true;
}

static bool mach_push_callee_saves(nyr_x64_mach_enc_t *e, unsigned mask) {
  /*
   * order: r12,r13,r14,r15,rbx
   */
  static const unsigned char ops[5][3] = {
      {2, 0x41, 0x54}, {2, 0x41, 0x55}, {2, 0x41, 0x56},
      {2, 0x41, 0x57}, {1, 0x53, 0x00}, /* push %rbx */
  };
  for (int i = 0; i < MIR_CALLEE_COLOR_N; ++i)
    if (mask & (1u << i)) {
      if (ops[i][0] == 2) {
        if (!mach_u8(e, ops[i][1]) || !mach_u8(e, ops[i][2]))
          return false;
      } else if (!mach_u8(e, ops[i][1]))
        return false;
    }
  return true;
}
static bool mach_pop_callee_saves(nyr_x64_mach_enc_t *e, unsigned mask) {
  /*
   * reverse: rbx, r15..r12
   */
  static const unsigned char ops[5][3] = {
      {2, 0x41, 0x5c}, {2, 0x41, 0x5d}, {2, 0x41, 0x5e},
      {2, 0x41, 0x5f}, {1, 0x5b, 0x00}, /* pop %rbx */
  };
  for (int i = MIR_CALLEE_COLOR_N - 1; i >= 0; --i)
    if (mask & (1u << i)) {
      if (ops[i][0] == 2) {
        if (!mach_u8(e, ops[i][1]) || !mach_u8(e, ops[i][2]))
          return false;
      } else if (!mach_u8(e, ops[i][1]))
        return false;
    }
  return true;
}
static bool mach_store_rax(nyr_x64_mach_enc_t *e, int off);
static bool mach_mov_reg_rax(nyr_x64_mach_enc_t *e, int preg);
static bool mach_mov_preg_preg(nyr_x64_mach_enc_t *e, int dst, int src);
static bool mach_mov_preg_imm(nyr_x64_mach_enc_t *e, unsigned preg,
                              int64_t imm);
static bool mach_load_home_preg(nyr_x64_mach_enc_t *e, int off, unsigned preg);

static bool mach_store_preg_home(nyr_x64_mach_enc_t *e, int preg, int off) {
  /*
   * movq %preg, off(%rbp).
   *
   * Vreg homes live below %rbp. A non-negative offset means the vreg never
   * got a home (mach_vreg_home_off's fallback is 0); emitting it would store
   * through the saved frame pointer and corrupt the caller's frame.
   */
  if (off >= 0)
    return mach_err(e, "x64 machine form encode: vreg has no stack home");
  unsigned char modrm = (unsigned char)(0x85 | ((preg & 7) << 3));
  return mach_rex(e, true, preg >= 8, false, false) && mach_u8(e, 0x89) &&
         mach_u8(e, modrm) && mach_i32(e, off);
}

static bool mach_vreg_has_ret_use(const nyr_x64_mach_enc_t *e, uint32_t vreg) {
  if (!e || !e->mach)
    return false;
  if (e->vreg_has_ret_use && vreg < e->mach->vreg_len)
    return e->vreg_has_ret_use[vreg];
  for (size_t i = 0; i < e->mach->inst_len; ++i) {
    const nyr_mach_inst_t *in = &e->mach->insts[i];
    if (in->opcode == NYR_MACH_RET && in->src0.kind == NYR_MACH_OPERAND_VREG &&
        in->src0.as.reg == vreg)
      return true;
  }
  return false;
}

static bool mach_load_call_arg_rax(nyr_x64_mach_enc_t *e,
                                   const nyr_mach_operand_t *arg) {
  return mach_load_op_rax(e, arg);
}

/*
 * Direct placement of an integer call argument into an ABI register.  The
 * caller (gp_direct classification) has already proven the source cannot
 * alias an ABI destination and does not rely on the transient %rax tags;
 * this routine only emits the move.
 */
static bool mach_load_call_arg_preg(nyr_x64_mach_enc_t *e,
                                    const nyr_mach_operand_t *arg,
                                    unsigned preg) {
  if (!e || !arg)
    return false;
  if (arg->kind == NYR_MACH_OPERAND_VREG) {
    /*
     * Calls flush colored values to their canonical homes before staging
     * arguments. Reload directly into each ABI destination: reseeding a
     * later argument into its assigned preg can overwrite an argument already
     * placed there when the preg aliases an earlier ABI slot.
     */
    return mach_load_home_preg(e, mach_vreg_home_off(e, arg->as.reg), preg);
  }
  if (arg->kind == NYR_MACH_OPERAND_IMM)
    return mach_mov_preg_imm(e, preg, arg->as.imm);
  if (arg->kind == NYR_MACH_OPERAND_FRAME)
    return mach_load_home_preg(e, mach_slot_off(e, arg), preg);
  return false;
}
/*
 * Fixed-register sequences and calls terminate ordinary GPR residency.
 * Callee-save colors are ABI-stable across calls, but keeping their homes
 * canonical here lets the fixed-register lowering use its scratch registers
 * without depending on a previous resident value.
 */
static bool mach_op_is_vreg(const nyr_mach_operand_t *op, uint32_t vreg);

static bool mach_flush_colored_to_home(nyr_x64_mach_enc_t *e) {
  if (!e || !e->color_seeded)
    return true;
  if (e->force_stack_reads)
    return true;
  bool saved = false;
  for (uint32_t v = 0; v < e->colors_len; ++v) {
    if (!e->color_seeded[v])
      continue;
    /*
     * A call/fixed-register barrier may split a live range and mark the
     * successor segment for reload even when the value is also live in a
     * later CFG block (or eventually returned).  In that case the canonical
     * home is exactly the hand-off between segments.  Skipping the store here
     * leaves the successor reload reading stale data.  Flush every resident
     * colored value; callee-saved physical registers may survive the call,
     * but the allocator's reload contract still requires the home to be
     * current.
     */
    const nyr_mach_live_segment_t *seg = mach_segment_at(e, v, e->cur_inst);
    if (!seg || seg->color < 0) {
      e->color_seeded[v] = false;
      continue;
    }
    if (!mach_store_preg_home(e, mach_color_preg[seg->color],
                              mach_vreg_home_off(e, v)))
      return false;
    e->color_seeded[v] = false;
    saved = true;
  }
  if (saved)
    mach_rax_invalidate(e);
  return true;
}

static void mach_invalidate_call_clobbered_colors(nyr_x64_mach_enc_t *e) {
  if (!e || !e->color_seeded)
    return;
  for (uint32_t v = 0; v < e->colors_len; ++v) {
    if (!e->color_seeded[v])
      continue;
    const nyr_mach_live_segment_t *seg = mach_segment_at(e, v, e->cur_inst);
    if (!seg || seg->color < 0 || seg->color >= MIR_CALLEE_COLOR_N)
      e->color_seeded[v] = false;
  }
}

/* Dedicated loop registers bridge canonical homes around calls. */
static bool mach_loop_gprs_call_home(nyr_x64_mach_enc_t *e, bool restore) {
  if (!e || !e->loop_gpr)
    return true;
  const nyr_mach_func_t *m = e->mach;
  size_t pc = e->cur_inst;
  size_t block_index = mach_block_for_inst(e, pc);
  if (block_index >= m->block_len)
    return true;
  const nyr_mach_block_t *block = &m->blocks[block_index];
  for (uint32_t v = 0; v < m->vreg_len; ++v) {
    int color = e->loop_gpr[v];
    if (color < 0 || color >= MIR_CALLEE_COLOR_N)
      continue;
    bool v_live_out = nyr_mach_regalloc_live_out(&e->regalloc, block_index, v);
    size_t v_start = nyr_mach_regalloc_live_in(&e->regalloc, block_index, v)
                         ? block->first_inst
                         : SIZE_MAX;
    size_t end = v_live_out ? block->first_inst + block->inst_count - 1 : 0;
    for (size_t i = 0; i < e->regalloc.segment_len; ++i) {
      const nyr_mach_live_segment_t *seg = &e->regalloc.segments[i];
      if (seg->vreg != v || seg->block != block_index)
        continue;
      if (seg->start < v_start)
        v_start = seg->start;
      if (seg->end > end)
        end = seg->end;
    }
    const nyr_mach_inst_t *call = &m->insts[pc];
    if (call->dst.kind == NYR_MACH_OPERAND_VREG && call->dst.as.reg == v)
      continue;
    bool live_after = end > pc || v_live_out;
    bool used_as_arg = false;
    for (size_t ai = 0; ai < call->args_len; ++ai) {
      if (call->args[ai].kind == NYR_MACH_OPERAND_VREG &&
          call->args[ai].as.reg == v) {
        used_as_arg = true;
        break;
      }
    }
    /*
     * Call arguments are read from canonical homes while the ABI argument
     * registers are staged. A loop-resident value whose last use is this
     * call is not live-out, but still needs its current value published
     * before staging. Values are reloaded after the call only when they are
     * genuinely live beyond it.
     */
    if (v_start == SIZE_MAX || v_start > pc ||
        (restore ? !live_after : !(live_after || used_as_arg)))
      continue;
    int preg = mach_color_preg[color];
    int off = mach_vreg_home_off(e, v);
    if (restore) {
      if (!mach_load_home_preg(e, off, (unsigned)preg))
        return false;
      if (e->color_seeded)
        e->color_seeded[v] = true;
    } else {
      if (!mach_store_preg_home(e, preg, off))
        return false;
      if (e->color_seeded)
        e->color_seeded[v] = false;
    }
  }
  return true;
}

/*
 * Spill only colored values whose physical registers a fixed-register
 * sequence will overwrite.  Reciprocal division owns R8-R10; flushing every
 * color there would turn a cheap loop operation into a full-function spill
 * barrier.
 */
static bool mach_flush_pregs_to_home(nyr_x64_mach_enc_t *e,
                                     unsigned preg_mask) {
  if (!e || !e->color_seeded || e->force_stack_reads)
    return true;
  bool saved = false;
  for (uint32_t v = 0; v < e->colors_len; ++v) {
    if (!e->color_seeded[v])
      continue;
    const nyr_mach_live_segment_t *seg = mach_segment_at(e, v, e->cur_inst);
    if (!seg || seg->color < 0) {
      e->color_seeded[v] = false;
      continue;
    }
    int preg = mach_color_preg[seg->color];
    if (preg < 0 || preg >= (int)(sizeof(preg_mask) * 8) ||
        !(preg_mask & (1u << preg)))
      continue;
    if (!mach_store_preg_home(e, preg, mach_vreg_home_off(e, v)))
      return false;
    e->color_seeded[v] = false;
    saved = true;
  }
  if (saved)
    mach_rax_invalidate(e);
  return true;
}

static bool mach_vreg_requires_home_after_def(nyr_x64_mach_enc_t *e,
                                              uint32_t vreg) {
  /*
   * Publish the canonical spill home after every definition that may feed a
   * later block. Loop-resident registers survive ordinary CFG edges, but not
   * every generated edge (notably cloned/alternate paths) proves that the
   * register alone is a valid home for all future consumers.
   */
  /*
   * A proven immediate consumer can use the just-computed RAX value even
   * when linear scan marked its segment spilled. Do not let the conservative
   * spill-home rule force a write that mach_commit_vreg will then forward.
   */
  if (mach_vreg_can_forward_to_next_rax(e, vreg) &&
      !mach_vreg_same_block_future_use_requires_home(e, vreg))
    return false;
  /*
   * Dedicated loop GPRs have one reserved physical register for every
   * segment of the vreg.  The allocator proves block interference and the
   * encoder seeds live-in blocks from that persistent register; calls flush
   * these values before crossing the ABI boundary.  This also covers return
   * operands: RET materializes the named value from its assigned preg.
   */
  if (e && e->loop_gpr && vreg < e->colors_len && e->loop_gpr[vreg] >= 0)
    return false;
  /*
   * A globally folded integer literal whose every use is an encodable RHS
   * immediate has no canonical-home consumer. Keep the fact across block
   * boundaries too: each use rematerializes the same SSA constant, so a
   * stack write here would only create an unread spill slot.
   */
  bool rematerialized_literal = e && e->mach && e->vreg_literal_rhs_only &&
                                vreg < e->mach->vreg_len &&
                                e->vreg_literal_rhs_only[vreg];
  /*
   * A def whose value is consumed in a later block must always keep its
   * canonical home current.  A carried segment elsewhere in the function
   * proves only that SOME edge delivers the preg — a join reached over a
   * non-carried path re-seeds from the home, so skipping the write here
   * would feed it a stale value.
   */
  return mach_vreg_has_ret_use(e, vreg) ||
         mach_vreg_future_use_requires_home(e, vreg) ||
         mach_vreg_same_block_future_use_requires_home(e, vreg) ||
         (!rematerialized_literal && mach_vreg_used_in_later_block(e, vreg));
}

static bool mach_commit_preg(nyr_x64_mach_enc_t *e,
                             const nyr_mach_operand_t *dst, int off,
                             unsigned preg, bool home_is_current) {
  if (!dst || dst->kind != NYR_MACH_OPERAND_VREG)
    return false;
  /*
   * A colored register may be reused after a live-range transition.  Clear
   * the seed bit for every older value sharing this physical register before
   * publishing the new value.
   */
  if (e->color_seeded) {
    for (uint32_t v = 0; v < e->colors_len; ++v) {
      if (v == dst->as.reg || !e->color_seeded[v])
        continue;
      int other = mach_vreg_preg(e, v);
      if (other == (int)preg)
        e->color_seeded[v] = false;
    }
  }
  if (mach_vreg_requires_home_after_def(e, dst->as.reg)) {
    /*
     * Later blocks re-seed colored registers from the canonical home.  A
     * frame load can already have materialized this exact home.
     */
    if (!home_is_current && !mach_store_preg_home(e, (int)preg, off))
      return false;
  }
  /*
   * RAX still holds the newly committed value, not necessarily the value
   * associated with the previous sticky-home tag. A following load of that
   * old home must not reuse the new value.
   */
  mach_rax_invalidate(e);
  if (e->color_seeded && dst->as.reg < e->colors_len)
    e->color_seeded[dst->as.reg] = true;
  return true;
}

static bool mach_commit_vreg(nyr_x64_mach_enc_t *e,
                             const nyr_mach_operand_t *dst, int off) {
  if (dst && dst->kind == NYR_MACH_OPERAND_VREG) {
    int preg = mach_vreg_def_preg(e, dst->as.reg);
    if (preg >= 0) {
      if (!mach_mov_reg_rax(e, preg))
        return false;
      return mach_commit_preg(e, dst, off, (unsigned)preg, false);
    }
  }
  if (dst && dst->kind == NYR_MACH_OPERAND_VREG && !e->no_sticky &&
      mach_is_i64(e->mach, dst) &&
      mach_vreg_can_forward_to_next_rax(e, dst->as.reg)) {
    e->pending_rax_off = off;
    e->pending_rax_valid = true;
    return true;
  }
  return mach_store_rax(e, off);
}

static bool mach_mov_reg_rax(nyr_x64_mach_enc_t *e, int preg) {
  /*
   * movq %rax, %preg.
   */
  if (preg == 10)
    mach_r10_index_invalidate(e);
  return mach_rex(e, true, false, false, preg >= 8) && mach_u8(e, 0x89) &&
         mach_u8(e, (unsigned char)(0xc0 | (preg & 7)));
}

static bool mach_mov_preg_preg(nyr_x64_mach_enc_t *e, int dst, int src) {
  if (dst == src)
    return true;
  return mach_rex(e, true, src >= 8, false, dst >= 8) && mach_u8(e, 0x89) &&
         mach_u8(e, (unsigned char)(0xc0 | ((src & 7) << 3) | (dst & 7)));
}

static bool mach_alu_preg_imm(nyr_x64_mach_enc_t *e, nyr_mach_opcode_t op,
                              int dst, int64_t imm) {
  if ((int64_t)(int32_t)imm != imm)
    return false;
  if (op == NYR_MACH_MUL) {
    /*
     * LEA is a single, low-latency integer multiply for the common small
     * constants 3, 5, and 9. LLVM's x86 selector makes the same choice for
     * these affine forms; keep the source and destination in one preg.
     */
    unsigned scale = imm == 3 ? 1 : imm == 5 ? 2 : imm == 9 ? 3 : 0;
    if (scale) {
      unsigned low = (unsigned)dst & 7u;
      /*
       * In mod=00 SIB encoding, base=101 means "no base" even when REX.B
       * extends it to r13. Use mod=01 plus a zero disp8 for rbp/r13 bases.
       */
      return mach_rex(e, true, dst >= 8, dst >= 8, dst >= 8) &&
             mach_u8(e, 0x8d) &&
             mach_u8(e, (low == 5 ? 0x44 : 0x04) | (low << 3)) &&
             mach_u8(e, (scale << 6) | (low << 3) | low) &&
             (low != 5 || mach_u8(e, 0));
    }
    return mach_rex(e, true, dst >= 8, false, dst >= 8) && mach_u8(e, 0x69) &&
           mach_u8(e, (unsigned char)(0xc0 | ((dst & 7) << 3) | (dst & 7))) &&
           mach_i32(e, (int32_t)imm);
  }
  unsigned ext = op == NYR_MACH_ADD   ? 0
                 : op == NYR_MACH_SUB ? 5
                 : op == NYR_MACH_AND ? 4
                 : op == NYR_MACH_OR  ? 1
                 : op == NYR_MACH_XOR ? 6
                                      : 0xff;
  if (ext == 0xff)
    return false;
  return mach_rex(e, true, false, false, dst >= 8) && mach_u8(e, 0x81) &&
         mach_u8(e, (unsigned char)(0xc0 | (ext << 3) | (dst & 7))) &&
         mach_i32(e, (int32_t)imm);
}

/* Emit dst = src * imm directly for the small LEA-friendly odd constants. */
static bool mach_mul_preg_imm_from_preg(nyr_x64_mach_enc_t *e, int dst, int src,
                                        int64_t imm) {
  unsigned scale = imm == 3 ? 1 : imm == 5 ? 2 : imm == 9 ? 3 : 0;
  if (dst < 0 || dst >= 16 || src < 0 || src >= 16 || !scale || src == 4)
    return false;
  unsigned low = (unsigned)src & 7u;
  unsigned mod = low == 5 ? 1u : 0u;
  if (!mach_rex(e, true, dst >= 8, src >= 8, src >= 8) || !mach_u8(e, 0x8d) ||
      !mach_u8(e, (mod << 6) | (((unsigned)dst & 7u) << 3) | 0x04u) ||
      !mach_u8(e, (scale << 6) | (low << 3) | low) ||
      (mod && !mach_u8(e, 0x00)))
    return false;
  if (dst == 10)
    mach_r10_index_invalidate(e);
  return true;
}

static bool mach_alu_preg_preg(nyr_x64_mach_enc_t *e, nyr_mach_opcode_t op,
                               int dst, int src) {
  unsigned char opc = op == NYR_MACH_ADD   ? 0x03
                      : op == NYR_MACH_SUB ? 0x2b
                      : op == NYR_MACH_AND ? 0x23
                      : op == NYR_MACH_OR  ? 0x0b
                      : op == NYR_MACH_XOR ? 0x33
                                           : 0;
  if (op == NYR_MACH_MUL) {
    return mach_rex(e, true, dst >= 8, false, src >= 8) && mach_u8(e, 0x0f) &&
           mach_u8(e, 0xaf) &&
           mach_u8(e, (unsigned char)(0xc0 | ((dst & 7) << 3) | (src & 7)));
  }
  if (!opc)
    return false;
  return mach_rex(e, true, dst >= 8, false, src >= 8) && mach_u8(e, opc) &&
         mach_u8(e, (unsigned char)(0xc0 | ((dst & 7) << 3) | (src & 7)));
}

/*
 * Load an operand's current value into a physical register using only the
 * trusted source channels (immediate rematerialization, sticky %rax,
 * seeded preg, canonical home through mach_load_op_rax).  Raw home reads
 * are not sound here: a def whose store was suppressed by rax forwarding
 * leaves a stale home, and only the load paths above know where the live
 * value actually is.
 */
static bool mach_load_src_preg(nyr_x64_mach_enc_t *e,
                               const nyr_mach_operand_t *op, unsigned preg) {
  if (!e || !op || preg >= 16)
    return false;
  if (op->kind == NYR_MACH_OPERAND_VREG) {
    if (e->imm_fold && e->imm_fold_known && op->as.reg < e->mach->vreg_len &&
        e->imm_fold_known[op->as.reg])
      return mach_mov_preg_imm(e, preg, e->imm_fold[op->as.reg]);
    if (e->addr_fold && e->addr_fold_known && op->as.reg < e->mach->vreg_len &&
        e->addr_fold_known[op->as.reg])
      return mach_remat_addr_preg(e, (int)preg, &e->addr_fold[op->as.reg]);
    int off = mach_slot_off(e, op);
    if (!e->no_sticky && e->last_rax_valid && e->last_rax_off == off)
      return mach_mov_preg_preg(e, (int)preg, 0);
    if (!mach_seed_colored_preg(e, op->as.reg))
      return false;
    int sp = mach_vreg_preg(e, op->as.reg);
    if (sp >= 0)
      return mach_mov_preg_preg(e, (int)preg, sp);
  }
  if (!mach_load_op_rax(e, op))
    return false;
  if (preg == 0)
    return true;
  return mach_mov_reg_rax(e, (int)preg);
}

/*
 * Immediate-count shift/rotate on a colored destination: shl/sar/rorq $imm,
 * %rdst.  Callers mask the count like the hardware would.
 */
static bool mach_shift_preg_imm(nyr_x64_mach_enc_t *e, nyr_mach_opcode_t op,
                                int dst, int64_t imm) {
  unsigned ext = op == NYR_MACH_SHL   ? 6
                 : op == NYR_MACH_SAR ? 7
                 : op == NYR_MACH_ROR ? 1
                                      : 0xff;
  if (ext == 0xff || imm < 0 || imm > 63)
    return false;
  return mach_rex(e, true, false, false, dst >= 8) && mach_u8(e, 0xc1) &&
         mach_u8(e, (unsigned char)(0xc0 | (ext << 3) | (dst & 7))) &&
         mach_u8(e, (unsigned char)imm);
}

static bool mach_pow2_divisor(const nyr_mach_operand_t *op,
                              const int64_t *imm_fold,
                              const bool *imm_fold_known, size_t vreg_len,
                              unsigned *shift_out) {
  int64_t divisor = 0;
  if (!op || !shift_out)
    return false;
  if (op->kind == NYR_MACH_OPERAND_IMM) {
    divisor = op->as.imm;
  } else if (op->kind == NYR_MACH_OPERAND_VREG && imm_fold && imm_fold_known &&
             op->as.reg < vreg_len && imm_fold_known[op->as.reg]) {
    divisor = imm_fold[op->as.reg];
  } else {
    return false;
  }
  if (divisor <= 1 || divisor > INT32_MAX ||
      ((uint64_t)divisor & ((uint64_t)divisor - 1)) != 0)
    return false;
  unsigned shift = 0;
  while (divisor > 1) {
    divisor >>= 1;
    ++shift;
  }
  if (shift >= 31)
    return false;
  *shift_out = shift;
  return true;
}

static bool mach_mov_rax_reg(nyr_x64_mach_enc_t *e, int preg) {
  /*
   * movq %preg, %rax — REX.B extends the source register.
   */
  if (!(mach_rex(e, true, false, false, preg >= 8) && mach_u8(e, 0x8b) &&
        mach_u8(e, (unsigned char)(0xc0 | (preg & 7)))))
    return false;
  mach_rax_invalidate(e);
  return true;
}

static bool mach_mov_rax_imm(nyr_x64_mach_enc_t *e, int64_t v) {
  mach_rax_invalidate(e);
  if (v == 0) {
    /*
     * xor eax,eax: 2B zero idiom; also breaks the dependency chain.
     */
    if (!(mach_u8(e, 0x31) && mach_u8(e, 0xc0)))
      return false;
  } else if ((uint64_t)v <= 0xffffffffu) {
    /*
     * mov eax,imm32: 5B, zero-extends into rax.
     */
    if (!(mach_u8(e, 0xb8) && mach_i32(e, (int32_t)(uint32_t)v)))
      return false;
  } else if (v < 0 && (int64_t)(int32_t)v == v) {
    /*
     * mov r/m64,imm32 with mod=11 sign-extends the compact immediate.
     */
    if (!(mach_rex(e, true, false, false, false) && mach_u8(e, 0xc7) &&
          mach_u8(e, 0xc0) && mach_i32(e, (int32_t)v)))
      return false;
  } else if (!(mach_rex(e, true, false, false, false) && mach_u8(e, 0xb8) &&
               mach_i64(e, v)))
    return false;
  /*
   * Immediate not tied to a slot until store.
   */
  return true;
}

static void mach_rax_invalidate(nyr_x64_mach_enc_t *e) {
  e->last_rax_valid = false;
  e->last_rax_off = 0;
}

static bool mach_load_rax(nyr_x64_mach_enc_t *e, int off) {
  /*
   * Skip reload when %rax already holds this slot (QBE-style copy elision).
   */
  if (!e->no_sticky && e->last_rax_valid && e->last_rax_off == off)
    return true;
  if (!(mach_rex(e, true, false, false, false) && mach_u8(e, 0x8b) &&
        mach_u8(e, 0x85) && mach_i32(e, off)))
    return false;
  e->last_rax_valid = true;
  e->last_rax_off = off;
  return true;
}

static bool mach_store_rax(nyr_x64_mach_enc_t *e, int off) {
  /*
   * A matching sticky-home tag means RAX still contains the value currently
   * stored at this exact stack slot.  Re-publishing it is a redundant
   * load/store pair; retain the tag and omit the store.
   */
  if (!e->no_sticky && e->last_rax_valid && e->last_rax_off == off)
    return true;
  if (!(mach_rex(e, true, false, false, false) && mach_u8(e, 0x89) &&
        mach_u8(e, 0x85) && mach_i32(e, off)))
    return false;
  e->last_rax_valid = true;
  e->last_rax_off = off;
  return true;
}

static bool mach_load_op_rax(nyr_x64_mach_enc_t *e,
                             const nyr_mach_operand_t *op) {
  if (op->kind == NYR_MACH_OPERAND_IMM)
    return mach_mov_rax_imm(e, op->as.imm);
  if (op->kind == NYR_MACH_OPERAND_VREG) {
    if (e->imm_fold && e->imm_fold_known && op->as.reg < e->mach->vreg_len &&
        e->imm_fold_known[op->as.reg])
      return mach_mov_rax_imm(e, e->imm_fold[op->as.reg]);
    if (e->addr_fold && e->addr_fold_known && op->as.reg < e->mach->vreg_len &&
        e->addr_fold_known[op->as.reg]) {
      mach_rax_invalidate(e);
      size_t start = e->code.len;
      bool ok = mach_remat_addr_preg(e, 0, &e->addr_fold[op->as.reg]);
      if (e->debug_name && ny_dbg_verbosity() >= 6 &&
          ny_dbg_function_selected(e->debug_name)) {
        const char *trace_filter = getenv("NY_TRACE_X64_SIB_FILTER");
        if (trace_filter && *trace_filter &&
            strstr(e->debug_name, trace_filter)) {
          ny_dbg_enter_context(e->debug_name);
          ny_dbg_heuristic_event(
              "X64_ADDR_REMAT",
              "inst=%zu block=%zu vreg=%u ok=%d emitted_bytes=%zu", e->cur_inst,
              e->cur_block, op->as.reg, (int)ok, e->code.len - start);
          ny_dbg_exit_context(e->debug_name);
        }
      }
      return ok;
    }
    if (!mach_seed_colored_preg(e, op->as.reg))
      return false;
    int preg = mach_vreg_preg(e, op->as.reg);
    if (preg >= 0) {
      /*
       * Sticky-rax applies to preg-resident values too: after a def was
       * published from %rax (or an earlier load left it there), %rax still
       * holds the same bits as the canonical home, so the preg->rax staging
       * move is redundant.
       */
      int off = mach_slot_off(e, op);
      if (!e->no_sticky && e->last_rax_valid && e->last_rax_off == off)
        return true;
      if (!mach_mov_rax_reg(e, preg))
        return false;
      /*
       * Dual-write keeps stack == preg; mark sticky to this home.
       */
      e->last_rax_valid = true;
      e->last_rax_off = off;
      return true;
    }
  }
  return mach_load_rax(e, mach_slot_off(e, op));
}

static bool mach_mov_preg_imm(nyr_x64_mach_enc_t *e, unsigned preg,
                              int64_t value) {
  if (preg >= 16)
    return false;
  if ((uint64_t)value <= UINT32_MAX) {
    if ((preg >= 8 && !mach_u8(e, 0x41)) ||
        !mach_u8(e, (unsigned char)(0xb8 + (preg & 7))) ||
        !mach_i32(e, (int32_t)(uint32_t)value))
      return false;
  } else if (!mach_rex(e, true, false, false, preg >= 8) ||
             !mach_u8(e, (unsigned char)(0xb8 + (preg & 7))) ||
             !mach_i64(e, value)) {
    return false;
  }
  if (preg == 0)
    mach_rax_invalidate(e);
  if (preg == 10)
    mach_r10_index_invalidate(e);
  return true;
}

static bool mach_load_home_preg(nyr_x64_mach_enc_t *e, int off, unsigned preg) {
  unsigned char modrm = (unsigned char)(0x85 | ((preg & 7) << 3));
  if (!mach_rex(e, true, preg >= 8, false, false) || !mach_u8(e, 0x8b) ||
      !mach_u8(e, modrm) || !mach_i32(e, off))
    return false;
  if (preg == 0)
    mach_rax_invalidate(e);
  if (preg == 10)
    mach_r10_index_invalidate(e);
  return true;
}

/*
 * Load a scalar store value into a chosen scratch preg without touching RAX,
 * which may be part of the already-resolved destination address.
 */
static bool mach_load_op_preg(nyr_x64_mach_enc_t *e,
                              const nyr_mach_operand_t *op, unsigned preg) {
  if (!e || !op || preg >= 16)
    return false;
  if (op->kind == NYR_MACH_OPERAND_IMM)
    return mach_mov_preg_imm(e, preg, op->as.imm);
  if (op->kind == NYR_MACH_OPERAND_FRAME)
    return mach_load_home_preg(e, mach_slot_off(e, op), preg);
  if (op->kind != NYR_MACH_OPERAND_VREG || op->as.reg >= e->colors_len)
    return false;
  uint32_t vreg = op->as.reg;
  if (e->imm_fold && e->imm_fold_known && e->imm_fold_known[vreg])
    return mach_mov_preg_imm(e, preg, e->imm_fold[vreg]);
  if (e->addr_fold && e->addr_fold_known && e->addr_fold_known[vreg]) {
    if (!mach_remat_addr_preg(e, (int)preg, &e->addr_fold[vreg]))
      return false;
    if (preg == 0)
      mach_rax_invalidate(e);
    if (preg == 10)
      mach_r10_index_invalidate(e);
    return true;
  }
  if (!mach_seed_colored_preg(e, vreg))
    return false;
  int src = mach_vreg_preg(e, vreg);
  if (src >= 0) {
    if (src != (int)preg && !mach_mov_preg_preg(e, (int)preg, src))
      return false;
    if (preg == 0)
      mach_rax_invalidate(e);
    if (preg == 10)
      mach_r10_index_invalidate(e);
    return true;
  }
  return mach_load_home_preg(e, mach_vreg_home_off(e, vreg), preg);
}

/*
 * Resolve the address operand of a memory access into a base register.
 * On success *base holds a 64-bit register whose contents are the
 * address; *base == 0 means rax (loaded here).  A colored preg is used
 * in place so each access skips the preg->rax copy; the sticky rax
 * cache is untouched in that case.
 */
static bool mach_addr_base(nyr_x64_mach_enc_t *e, const nyr_mach_operand_t *op,
                           unsigned *base) {
  if (!e || !op || !base)
    return false;
  if (op->kind == NYR_MACH_OPERAND_IMM) {
    if (!mach_mov_rax_imm(e, op->as.imm))
      return false;
    *base = 0;
    return true;
  }
  if (op->kind != NYR_MACH_OPERAND_VREG || op->as.reg >= e->mach->vreg_len)
    return false;
  if (e->imm_fold && e->imm_fold_known && e->imm_fold_known[op->as.reg]) {
    if (!mach_mov_rax_imm(e, e->imm_fold[op->as.reg]))
      return false;
    *base = 0;
    return true;
  }
  if (e->addr_fold && e->addr_fold_known && e->addr_fold_known[op->as.reg]) {
    mach_rax_invalidate(e);
    if (!mach_remat_addr_preg(e, 0, &e->addr_fold[op->as.reg]))
      return false;
    *base = 0;
    return true;
  }
  if (!mach_seed_colored_preg(e, op->as.reg))
    return false;
  int preg = mach_vreg_preg(e, op->as.reg);
  if (preg >= 0) {
    *base = (unsigned)preg;
    return true;
  }
  int off = mach_slot_off(e, op);
  if (!mach_load_rax(e, off))
    return false;
  e->last_rax_valid = true;
  e->last_rax_off = off;
  *base = 0;
  return true;
}

/*
 * Resolved memory operand: [base] or [base + index*scale].
 */
typedef struct {
  unsigned base;
  int index; /* -1: no index register */
  unsigned scale;
  int32_t disp;
} ny_x64_mem_addr_t;

/*
 * Resolve a single-use indexed LEA used by the immediately following load.
 * The address vreg can be omitted when its inputs are already resident and
 * encoded directly as a SIB operand.
 */
static bool mach_inst_uses_vreg(const nyr_mach_inst_t *in, uint32_t vreg);
static size_t mach_inst_input_uses_vreg(const nyr_mach_inst_t *in,
                                        uint32_t vreg);

static void mach_r10_index_invalidate(nyr_x64_mach_enc_t *e) {
  e->r10_index_valid = false;
}

static bool mach_sib_lea_mem(const nyr_x64_mach_enc_t *e, uint32_t vreg,
                             size_t use_index,
                             const nyr_mach_operand_t **base_out,
                             const nyr_mach_operand_t **index_out,
                             unsigned *scale_out, const char **reject_reason) {
#define SIB_LEA_REJECT(reason)                                                 \
  do {                                                                         \
    if (reject_reason)                                                         \
      *reject_reason = reason;                                                 \
    return false;                                                              \
  } while (0)
  if (reject_reason)
    *reject_reason = NULL;
  if (!e || !e->mach || !e->def_inst || vreg >= e->mach->vreg_len ||
      use_index >= e->mach->inst_len)
    SIB_LEA_REJECT("invalid query");
  size_t def_index = e->def_inst[vreg];
  if (def_index == SIZE_MAX || def_index + 1 != use_index)
    SIB_LEA_REJECT(def_index == SIZE_MAX ? "definition unavailable"
                                         : "not adjacent");
  size_t def_block = mach_block_for_inst(e, def_index);
  if (def_block == SIZE_MAX || mach_block_for_inst(e, use_index) != def_block)
    SIB_LEA_REJECT("cross-block");
  const nyr_mach_inst_t *def = &e->mach->insts[def_index];
  const nyr_mach_inst_t *use = &e->mach->insts[use_index];
  bool is_load =
      use->opcode == NYR_MACH_LOAD && mach_op_is_vreg(&use->src0, vreg);
  bool is_store =
      use->opcode == NYR_MACH_STORE && mach_op_is_vreg(&use->dst, vreg);
  if (def->opcode != NYR_MACH_LEA || def->dst.kind != NYR_MACH_OPERAND_VREG ||
      def->dst.as.reg != vreg || def->src0.kind != NYR_MACH_OPERAND_VREG ||
      def->src1.kind != NYR_MACH_OPERAND_VREG ||
      def->src2.kind != NYR_MACH_OPERAND_IMM ||
      (def->src2.as.imm != 2 && def->src2.as.imm != 4 &&
       def->src2.as.imm != 8) ||
      (!is_load && !is_store))
    SIB_LEA_REJECT("unsupported LEA/use shape");
  /* Eliding the LEA is valid only if this is its sole use. In particular,
   * load/store pairs need the address value to remain materialized between
   * the two operations. */
  for (size_t i = def_index + 1; i < e->mach->inst_len; ++i) {
    if (i != use_index && mach_inst_uses_vreg(&e->mach->insts[i], vreg))
      SIB_LEA_REJECT("address has another use");
  }
  if (base_out)
    *base_out = &def->src0;
  if (index_out)
    *index_out = &def->src1;
  if (scale_out)
    *scale_out = (unsigned)def->src2.as.imm;
#undef SIB_LEA_REJECT
  return true;
}

/* Decompose an indexed LEA at a memory use even when the address also feeds
 * another memory operation (for example, the load and store of an array
 * element swap).  Unlike mach_sib_lea_mem(), this never licenses deleting or
 * moving the LEA: it only lets this one access encode its address as SIB. */
static bool mach_sib_lea_mem_use(const nyr_x64_mach_enc_t *e, uint32_t vreg,
                                 size_t use_index,
                                 const nyr_mach_operand_t **base_out,
                                 const nyr_mach_operand_t **index_out,
                                 unsigned *scale_out) {
  if (!e || !e->mach || !e->def_inst || vreg >= e->mach->vreg_len ||
      use_index >= e->mach->inst_len)
    return false;
  size_t def_index = e->def_inst[vreg];
  if (def_index == SIZE_MAX || def_index >= use_index)
    return false;
  size_t block = mach_block_for_inst(e, def_index);
  if (block == SIZE_MAX || mach_block_for_inst(e, use_index) != block)
    return false;
  const nyr_mach_inst_t *def = &e->mach->insts[def_index];
  const nyr_mach_inst_t *use = &e->mach->insts[use_index];
  bool is_load =
      use->opcode == NYR_MACH_LOAD && mach_op_is_vreg(&use->src0, vreg);
  bool is_store =
      use->opcode == NYR_MACH_STORE && mach_op_is_vreg(&use->dst, vreg);
  if (def->opcode != NYR_MACH_LEA || def->dst.kind != NYR_MACH_OPERAND_VREG ||
      def->dst.as.reg != vreg || def->src0.kind != NYR_MACH_OPERAND_VREG ||
      def->src1.kind != NYR_MACH_OPERAND_VREG ||
      def->src2.kind != NYR_MACH_OPERAND_IMM ||
      (def->src2.as.imm != 2 && def->src2.as.imm != 4 &&
       def->src2.as.imm != 8) ||
      (!is_load && !is_store))
    return false;
  if (base_out)
    *base_out = &def->src0;
  if (index_out)
    *index_out = &def->src1;
  if (scale_out)
    *scale_out = (unsigned)def->src2.as.imm;
  return true;
}

/* A multi-use address LEA may be omitted only if every use is itself a
 * memory address in this block.  Each access can then reconstruct the same
 * SIB address directly from the LEA's SSA inputs. */
static bool mach_sib_lea_all_uses_mem(const nyr_x64_mach_enc_t *e,
                                      uint32_t vreg, size_t def_index) {
  if (!e || !e->mach || vreg >= e->mach->vreg_len ||
      def_index >= e->mach->inst_len)
    return false;
  size_t block = mach_block_for_inst(e, def_index);
  if (block == SIZE_MAX)
    return false;
  bool found = false;
  for (size_t i = def_index + 1; i < e->mach->inst_len; ++i) {
    const nyr_mach_inst_t *in = &e->mach->insts[i];
    if (!mach_inst_uses_vreg(in, vreg))
      continue;
    bool is_load =
        in->opcode == NYR_MACH_LOAD && mach_op_is_vreg(&in->src0, vreg);
    bool is_store =
        in->opcode == NYR_MACH_STORE && mach_op_is_vreg(&in->dst, vreg);
    if ((!is_load && !is_store) || mach_block_for_inst(e, i) != block)
      return false;
    found = true;
  }
  return found;
}

static bool mach_sib_add_mem(const nyr_x64_mach_enc_t *e, uint32_t vreg,
                             size_t use_index,
                             const nyr_mach_operand_t **base_out,
                             const nyr_mach_operand_t **index_out,
                             unsigned *scale_out,
                             const nyr_mach_operand_t **scaled_value_out) {
  if (!e || !e->mach || !e->def_inst || vreg >= e->mach->vreg_len ||
      use_index >= e->mach->inst_len)
    return false;
  size_t def_index = e->def_inst[vreg];
  if (def_index == SIZE_MAX || def_index + 1 != use_index)
    return false;
  size_t block = mach_block_for_inst(e, def_index);
  if (block == SIZE_MAX || mach_block_for_inst(e, use_index) != block)
    return false;
  const nyr_mach_inst_t *def = &e->mach->insts[def_index];
  const nyr_mach_inst_t *use = &e->mach->insts[use_index];
  bool is_load =
      use->opcode == NYR_MACH_LOAD && mach_op_is_vreg(&use->src0, vreg);
  bool is_store =
      use->opcode == NYR_MACH_STORE && mach_op_is_vreg(&use->dst, vreg);
  if (def->opcode != NYR_MACH_ADD || def->dst.kind != NYR_MACH_OPERAND_VREG ||
      def->dst.as.reg != vreg || def->src0.kind != NYR_MACH_OPERAND_VREG ||
      def->src1.kind != NYR_MACH_OPERAND_VREG || (!is_load && !is_store))
    return false;
  const nyr_mach_operand_t *inputs[] = {&def->src0, &def->src1};
  const nyr_mach_operand_t *base = NULL, *index = NULL;
  const nyr_mach_operand_t *scaled_value = NULL;
  unsigned scale = 1;
  for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
    const nyr_mach_operand_t *input = inputs[i];
    if (input->as.reg >= e->mach->vreg_len)
      return false;
    size_t input_def = e->def_inst[input->as.reg];
    const nyr_mach_inst_t *producer =
        input_def != SIZE_MAX && input_def < e->mach->inst_len
            ? &e->mach->insts[input_def]
            : NULL;
    bool scaled = false;
    unsigned candidate_scale = 0;
    if (producer && producer->dst.kind == NYR_MACH_OPERAND_VREG &&
        producer->dst.as.reg == input->as.reg &&
        producer->src0.kind == NYR_MACH_OPERAND_VREG &&
        producer->src1.kind == NYR_MACH_OPERAND_IMM) {
      if (producer->opcode == NYR_MACH_SHL && producer->src1.as.imm >= 1 &&
          producer->src1.as.imm <= 3) {
        scaled = true;
        candidate_scale = 1u << producer->src1.as.imm;
      } else if (producer->opcode == NYR_MACH_MUL &&
                 (producer->src1.as.imm == 2 || producer->src1.as.imm == 4 ||
                  producer->src1.as.imm == 8)) {
        scaled = true;
        candidate_scale = (unsigned)producer->src1.as.imm;
      }
    }
    if (scaled) {
      if (index)
        return false;
      index = &producer->src0;
      scaled_value = input;
      scale = candidate_scale;
    } else if (!base) {
      base = input;
    } else if (!index) {
      index = input;
    } else {
      return false;
    }
  }
  if (!base || !index)
    return false;
  const nyr_mach_operand_t *other_uses[] = {is_load ? &use->src1 : &use->src0,
                                            &use->src1, &use->src2};
  for (size_t i = 0; i < sizeof(other_uses) / sizeof(other_uses[0]); ++i)
    if (mach_op_is_vreg(other_uses[i], vreg))
      return false;
  for (size_t i = use_index + 1; i < e->mach->inst_len; ++i)
    if (mach_inst_uses_vreg(&e->mach->insts[i], vreg))
      return false;
  if (base_out)
    *base_out = base;
  if (index_out)
    *index_out = index;
  if (scale_out)
    *scale_out = scale;
  if (scaled_value_out)
    *scaled_value_out = scaled_value;
  return true;
}

/*
 * A scaled index can be folded without materializing its SHL result only
 * when the following ADD is its sole machine-IR consumer. The ADD fold then
 * uses the original index and the x86 SIB scale directly.
 */
static bool mach_sib_scaled_index_only_feeds_add(const nyr_x64_mach_enc_t *e,
                                                 uint32_t vreg) {
  if (!e || !e->mach || vreg >= e->mach->vreg_len)
    return false;
  bool found = false;
  for (size_t i = 0; i < e->mach->inst_len; ++i) {
    const nyr_mach_inst_t *user = &e->mach->insts[i];
    bool used =
        mach_inst_input_uses_vreg(user, vreg) != 0 ||
        (user->opcode == NYR_MACH_STORE && mach_op_is_vreg(&user->dst, vreg));
    if (used) {
      const nyr_mach_inst_t *add = user;
      const nyr_mach_operand_t *scaled_value = NULL;
      if (add->opcode != NYR_MACH_ADD ||
          mach_inst_input_uses_vreg(add, vreg) != 1 ||
          add->dst.kind != NYR_MACH_OPERAND_VREG ||
          i + 1 >= e->mach->inst_len ||
          !mach_sib_add_mem(e, add->dst.as.reg, i + 1, NULL, NULL, NULL,
                            &scaled_value) ||
          !mach_op_is_vreg(scaled_value, vreg))
        return false;
      found = true;
    }
  }
  return found;
}

/*
 * Query decomposition of an address vreg into base/index/scale physical
 * registers. A single-use LEA feeding an adjacent load/store is
 * rematerialized into the reserved RAX/R10 scratch pair.
 */
static bool mach_sib_query(nyr_x64_mach_enc_t *e,
                           const nyr_mach_operand_t *addr, unsigned *base_preg,
                           unsigned *idx_preg, unsigned *scale) {
  if (e->pending_sib_valid && addr &&
      mach_op_is_vreg(addr, e->pending_sib_vreg)) {
    *base_preg = e->pending_sib_base;
    *idx_preg = e->pending_sib_index;
    *scale = e->pending_sib_scale;
    e->pending_sib_valid = false;
    return true;
  }
  if (!e->def_inst || !addr || addr->kind != NYR_MACH_OPERAND_VREG ||
      addr->as.reg >= e->mach->vreg_len)
    return false;
  size_t di = e->def_inst[addr->as.reg];
  if (di == SIZE_MAX || di >= e->mach->inst_len)
    return false;
  const nyr_mach_inst_t *d = &e->mach->insts[di];
  const nyr_mach_operand_t *lea_base = NULL, *lea_index = NULL;
  unsigned lea_scale = 0;
  const char *reject_reason = NULL;
  bool indexed_lea = mach_sib_lea_mem(e, addr->as.reg, e->cur_inst, &lea_base,
                                      &lea_index, &lea_scale, &reject_reason);
  if (!indexed_lea)
    indexed_lea = mach_sib_lea_mem_use(e, addr->as.reg, e->cur_inst, &lea_base,
                                       &lea_index, &lea_scale);
  if (e->debug_name && ny_dbg_verbosity() >= 6 &&
      ny_dbg_function_selected(e->debug_name)) {
    const char *trace_filter = getenv("NY_TRACE_X64_SIB_FILTER");
    size_t def_index = e->def_inst[addr->as.reg];
    const nyr_mach_inst_t *def =
        def_index != SIZE_MAX && def_index < e->mach->inst_len
            ? &e->mach->insts[def_index]
            : NULL;
    if (trace_filter && *trace_filter && strstr(e->debug_name, trace_filter) &&
        def && def->opcode == NYR_MACH_LEA &&
        def->src0.kind == NYR_MACH_OPERAND_VREG) {
      uint32_t base_vreg = def->src0.as.reg;
      int resident_base_preg = mach_vreg_preg(e, base_vreg);
      bool base_rematerializable = e->addr_fold_known &&
                                   base_vreg < e->mach->vreg_len &&
                                   e->addr_fold_known[base_vreg];
      ny_dbg_enter_context(e->debug_name);
      ny_dbg_heuristic_event(
          "X64_SIB_LEA",
          "inst=%zu block=%zu addr=v%u def=%zu eligible=%d reject=%s "
          "base=v%u preg=%d addr_remat=%d index=v%u scale=%u",
          e->cur_inst, e->cur_block, addr->as.reg, def_index, (int)indexed_lea,
          reject_reason ? reject_reason : "-", base_vreg, resident_base_preg,
          (int)base_rematerializable,
          indexed_lea ? lea_index->as.reg : def->src1.as.reg,
          indexed_lea ? lea_scale
                      : (def->src2.kind == NYR_MACH_OPERAND_IMM
                             ? (unsigned)def->src2.as.imm
                             : 0));
      ny_dbg_exit_context(e->debug_name);
    }
  }
  if (indexed_lea) {
    int bp = mach_vreg_preg(e, lea_base->as.reg);
    int ip = mach_vreg_preg(e, lea_index->as.reg);
    /* Only consume values whose current segments are already resident.
     * A skipped LEA can outlive either input's segment, in which case its
     * canonical-home fallback below is still required. R12 is a valid SIB
     * index with REX.X set; only RSP (register 4) means no index. */
    if (bp >= 0 && ip >= 0 && ip != 4) {
      *base_preg = (unsigned)bp;
      *idx_preg = (unsigned)ip;
      *scale = lea_scale;
      return true;
    }
    if (!e->r10_index_valid || e->r10_index_vreg != lea_index->as.reg) {
      if (!mach_load_op_rax(e, lea_index) || !mach_mov_reg_rax(e, 10))
        return false;
      e->r10_index_vreg = lea_index->as.reg;
      e->r10_index_valid = true;
    }
    if (!mach_load_op_rax(e, lea_base))
      return false;
    *base_preg = 0;
    *idx_preg = 10;
    *scale = lea_scale;
    return true;
  }
  if (d->opcode != NYR_MACH_ADD || d->dst.kind != NYR_MACH_OPERAND_VREG ||
      d->dst.as.reg != addr->as.reg)
    return false;

  const nyr_mach_operand_t *side[2] = {&d->src0, &d->src1};
  const nyr_mach_operand_t *bop = NULL, *iop = NULL;
  unsigned sc = 1;
  for (int k = 0; k < 2; ++k) {
    const nyr_mach_operand_t *s = side[k];
    if (s->kind != NYR_MACH_OPERAND_VREG || s->as.reg >= e->mach->vreg_len)
      return false;
    unsigned cand = 0;
    bool scaled = false;
    size_t si = e->def_inst[s->as.reg];
    if (si != SIZE_MAX && si < e->mach->inst_len) {
      const nyr_mach_inst_t *sd = &e->mach->insts[si];
      if (sd->dst.kind == NYR_MACH_OPERAND_VREG &&
          sd->dst.as.reg == s->as.reg &&
          sd->src0.kind == NYR_MACH_OPERAND_VREG &&
          sd->src1.kind == NYR_MACH_OPERAND_IMM) {
        if (sd->opcode == NYR_MACH_SHL && sd->src1.as.imm >= 1 &&
            sd->src1.as.imm <= 3) {
          scaled = true;
          cand = 1u << sd->src1.as.imm;
        } else if (sd->opcode == NYR_MACH_MUL &&
                   (sd->src1.as.imm == 2 || sd->src1.as.imm == 4 ||
                    sd->src1.as.imm == 8)) {
          scaled = true;
          cand = (unsigned)sd->src1.as.imm;
        }
      }
    }
    if (scaled) {
      /*
       * Index is the SHL/MUL input; its def site seeds the preg.
       */
      if (iop)
        return false;
      iop = &e->mach->insts[e->def_inst[s->as.reg]].src0;
      sc = cand;
    } else if (!bop) {
      bop = s;
    } else if (!iop) {
      iop = s; /* two plain vregs: scale 1 */
    } else {
      return false;
    }
  }
  if (!bop || !iop)
    return false;

  if (!mach_seed_colored_preg(e, bop->as.reg))
    return false;
  if (!mach_seed_colored_preg(e, iop->as.reg))
    return false;
  int bp = mach_vreg_preg(e, bop->as.reg);
  int ip = mach_vreg_preg(e, iop->as.reg);
  if (bp >= 0 && ip >= 0 && (ip & 7) != 4) {
    *base_preg = (unsigned)bp;
    *idx_preg = (unsigned)ip;
    *scale = sc;
    return true;
  }

  /*
   * Partial residency (colored side resident, the other in RAX/R10 scratch)
   * is intentionally not attempted: the scratch discipline interacts with
   * fixed-register sequences and carried induction values in ways the
   * segment model does not track yet, and miscompiles were observed on
   * row/column stencil loops. Full residency (both sides colored) is the
   * only SIB shape the vector paths consume.
   */
  if (e->cur_inst >= e->mach->inst_len ||
      (e->mach->insts[e->cur_inst].opcode != NYR_MACH_LOAD &&
       e->mach->insts[e->cur_inst].opcode != NYR_MACH_STORE))
    return false;

  const nyr_mach_operand_t *add_base = NULL, *add_index = NULL;
  unsigned add_scale = 0;
  if (e->cur_inst >= e->mach->inst_len ||
      e->mach->insts[e->cur_inst].opcode != NYR_MACH_LOAD)
    return false;
  if (!mach_sib_add_mem(e, addr->as.reg, e->cur_inst, &add_base, &add_index,
                        &add_scale, NULL) ||
      sc != add_scale)
    return false;
  const nyr_mach_operand_t *inputs[] = {add_base, add_index};
  for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
    const nyr_mach_operand_t *input = inputs[i];
    if (input->kind != NYR_MACH_OPERAND_VREG ||
        input->as.reg >= e->mach->vreg_len)
      return false;
    uint32_t vreg = input->as.reg;
    bool rematerializable =
        (e->imm_fold && e->imm_fold_known && e->imm_fold_known[vreg]) ||
        (e->addr_fold && e->addr_fold_known && e->addr_fold_known[vreg]);
    if (!mach_segment_at(e, vreg, e->cur_inst) && !rematerializable)
      return false;
  }
  if (!mach_load_op_rax(e, add_index) || !mach_mov_reg_rax(e, 10) ||
      !mach_load_op_rax(e, add_base))
    return false;
  *base_preg = 0;
  *idx_preg = 10;
  *scale = add_scale;
  return true;
}

/*
 * Resolve a memory-access address operand.  Prefers SIB form
 * [base + index*scale] when the defining ADD decomposes; otherwise falls
 * back to a single base register (rax when no colored preg exists).
 */
static bool mach_resolve_mem_addr(nyr_x64_mach_enc_t *e,
                                  const nyr_mach_operand_t *op,
                                  ny_x64_mem_addr_t *out) {
  out->base = 0;
  out->index = -1;
  out->scale = 1;
  out->disp = 0;
  if (op && op->kind == NYR_MACH_OPERAND_VREG &&
      op->as.reg < e->mach->vreg_len) {
    const nyr_mach_operand_t *base = NULL, *index = NULL;
    unsigned scale = 0;
    if (mach_sib_lea_mem(e, op->as.reg, e->cur_inst, &base, &index, &scale,
                         NULL) &&
        base->kind == NYR_MACH_OPERAND_VREG &&
        base->as.reg < e->mach->vreg_len &&
        index->kind == NYR_MACH_OPERAND_VREG && e->addr_fold &&
        e->addr_fold_known && e->addr_fold_known[base->as.reg] &&
        e->addr_fold[base->as.reg].kind == NYR_MACH_OPERAND_FRAME) {
      int index_preg = mach_vreg_preg(e, index->as.reg);
      if (index_preg < 0 || index_preg == 4) {
        if (!mach_load_op_rax(e, index) || !mach_mov_reg_rax(e, 10))
          return false;
        index_preg = 10;
      }
      out->base = 5; /* rbp */
      out->index = index_preg;
      out->scale = scale;
      out->disp = mach_slot_off(e, &e->addr_fold[base->as.reg]);
      if (e->debug_name && ny_dbg_verbosity() >= 6 &&
          ny_dbg_function_selected(e->debug_name)) {
        const char *trace_filter = getenv("NY_TRACE_X64_SIB_FILTER");
        if (trace_filter && *trace_filter &&
            strstr(e->debug_name, trace_filter)) {
          ny_dbg_enter_context(e->debug_name);
          ny_dbg_heuristic_event(
              "X64_SIB_FRAME",
              "inst=%zu block=%zu addr=v%u base=v%u index=v%u preg=%d "
              "scale=%u disp=%d",
              e->cur_inst, e->cur_block, op->as.reg, base->as.reg,
              index->as.reg, index_preg, scale, out->disp);
          ny_dbg_exit_context(e->debug_name);
        }
      }
      return true;
    }
  }
  unsigned b, i, s;
  if (mach_sib_query(e, op, &b, &i, &s)) {
    out->base = b;
    out->index = (int)i;
    out->scale = s;
    return true;
  }
  return mach_addr_base(e, op, &out->base);
}

static bool mach_store_value_preg(nyr_x64_mach_enc_t *e,
                                  const ny_x64_mem_addr_t *addr,
                                  const nyr_mach_operand_t *value,
                                  unsigned *preg_out) {
  static const unsigned candidates[] = {11, 8,  9,  1,  2,  6, 7,
                                        3,  12, 13, 14, 15, 0, 10};
  if (!e || !addr || !value || !preg_out)
    return false;
  /* A store reads its address and value without clobbering either register.
   * Use a resident value directly, including when it is also an address
   * operand; moving it to a scratch would only introduce copies and spills. */
  if (value->kind == NYR_MACH_OPERAND_VREG) {
    int preg = mach_vreg_preg(e, value->as.reg);
    if (preg >= 0) {
      *preg_out = (unsigned)preg;
      return true;
    }
  }
  for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
    unsigned preg = candidates[i];
    if (preg == addr->base ||
        (addr->index >= 0 && preg == (unsigned)addr->index))
      continue;
    /*
     * The address is already resolved. Commit any live value in the chosen
     * scratch register to its canonical home before reusing that register.
     */
    if (!mach_flush_pregs_to_home(e, 1u << preg) ||
        !mach_load_op_preg(e, value, preg))
      return false;
    *preg_out = preg;
    return true;
  }
  return false;
}

/*
 * REX with W/R plus X (index) and B (base) derived from the address.
 */
static bool mach_rex_for_addr(nyr_x64_mach_enc_t *e, const ny_x64_mem_addr_t *a,
                              bool w, bool r) {
  bool xx = a->index >= 0 && (unsigned)a->index >= 8;
  bool bx = a->base >= 8 || (xx && false);
  return mach_rex(e, w, r, xx, bx);
}

static bool mach_mem_modrm(nyr_x64_mach_enc_t *e, unsigned base, unsigned reg,
                           bool rex_w);

/*
 * ModRM/SIB/disp for a resolved address with a given reg field.
 */
static bool mach_emit_mem(nyr_x64_mach_enc_t *e, const ny_x64_mem_addr_t *a,
                          unsigned reg) {
  if (a->index < 0 && a->disp == 0)
    return mach_mem_modrm(e, a->base, reg, false);
  if (a->index < 0) {
    unsigned mod = a->disp >= INT8_MIN && a->disp <= INT8_MAX ? 1u : 2u;
    if (!mach_u8(
            e, (unsigned char)((mod << 6) | ((reg & 7) << 3) | (a->base & 7))))
      return false;
    return mod == 1 ? mach_u8(e, (unsigned char)a->disp) : mach_i32(e, a->disp);
  }
  unsigned log2 = a->scale == 2   ? 1u
                  : a->scale == 4 ? 2u
                  : a->scale == 8 ? 3u
                                  : 0u;
  unsigned mod = a->disp == 0
                     ? (((a->base & 7) == 5 || (a->base & 7) == 4) ? 1u : 0u)
                     : (a->disp >= INT8_MIN && a->disp <= INT8_MAX ? 1u : 2u);
  if (!mach_u8(e, (unsigned char)((mod << 6) | ((reg & 7) << 3) | 0x04)))
    return false;
  if (!mach_u8(e,
               (unsigned char)((log2 << 6) | (((unsigned)a->index & 7) << 3) |
                               (a->base & 7))))
    return false;
  if (mod == 1)
    return mach_u8(e, mod && a->disp == 0 ? 0x00 : (unsigned char)a->disp);
  if (mod == 2)
    return mach_i32(e, a->disp);
  return true;
}

/*
 * ModRM (+ SIB/disp fixups) for [base] with a given reg field and no
 * index.  rsp/r12 need a SIB byte; rbp/r13 cannot encode mod=00.
 */
static bool mach_mem_modrm(nyr_x64_mach_enc_t *e, unsigned base, unsigned reg,
                           bool rex_w) {
  (void)rex_w;
  if ((base & 7) == 4)
    return mach_u8(e, (unsigned char)((reg << 3) | 0x04)) && mach_u8(e, 0x24);
  if ((base & 7) == 5)
    return mach_u8(e, (unsigned char)(0x40 | (reg << 3) | (base & 7))) &&
           mach_u8(e, 0x00);
  return mach_u8(e, (unsigned char)((reg << 3) | (base & 7)));
}

static bool mach_add_patch(nyr_x64_mach_enc_t *e, uint32_t block) {
  if (e->patch_count == e->patch_cap) {
    size_t nc = e->patch_cap ? e->patch_cap * 2 : 32;
    size_t *na = realloc(e->patch_at, nc * sizeof(*na));
    uint32_t *nb = realloc(e->patch_block, nc * sizeof(*nb));
    if (!na || !nb) {
      free(na);
      free(nb);
      return mach_err(e, "x64 machine form encode: patch OOM");
    }
    e->patch_at = na;
    e->patch_block = nb;
    e->patch_cap = nc;
  }
  e->patch_at[e->patch_count] = e->code.len;
  e->patch_block[e->patch_count] = block;
  e->patch_count++;
  return true;
}

static bool mach_add_reloc(nyr_x64_mach_enc_t *e, const char *sym, int type) {
  if (e->reloc_count == e->reloc_cap) {
    size_t nc = e->reloc_cap ? e->reloc_cap * 2 : 16;
    nyr_x64_obj_reloc_t *nr = realloc(e->relocs, nc * sizeof(*nr));
    if (!nr)
      return mach_err(e, "x64 machine form encode: reloc OOM");
    e->relocs = nr;
    e->reloc_cap = nc;
  }
  nyr_x64_obj_reloc_t *r = &e->relocs[e->reloc_count++];
  memset(r, 0, sizeof(*r));
  snprintf(r->symbol, sizeof(r->symbol), "%s", sym ? sym : "");
  r->disp_off = e->code.len;
  r->type = type;
  return true;
}

static bool mach_is_i64(const nyr_mach_func_t *m,
                        const nyr_mach_operand_t *op) {
  return op && op->kind == NYR_MACH_OPERAND_VREG && op->as.reg < m->vreg_len &&
         (m->vreg_types[op->as.reg] == NYR_MACH_TYPE_I64 ||
          m->vreg_types[op->as.reg] == NYR_MACH_TYPE_PTR);
}

static bool mach_is_f64(const nyr_mach_func_t *m,
                        const nyr_mach_operand_t *op) {
  if (!op)
    return false;
  if (op->kind == NYR_MACH_OPERAND_VREG)
    return op->as.reg < m->vreg_len &&
           m->vreg_types[op->as.reg] == NYR_MACH_TYPE_F64;
  if (op->kind == NYR_MACH_OPERAND_FRAME)
    return op->as.frame_index < m->frame_slot_len &&
           m->frame_slots[op->as.frame_index].type == NYR_MACH_TYPE_F64;
  return false;
}

static bool mach_is_f32(const nyr_mach_func_t *m,
                        const nyr_mach_operand_t *op) {
  return op && op->kind == NYR_MACH_OPERAND_VREG && op->as.reg < m->vreg_len &&
         m->vreg_types[op->as.reg] == NYR_MACH_TYPE_F32;
}

static bool mach_is_float(const nyr_mach_func_t *m,
                          const nyr_mach_operand_t *op) {
  return mach_is_f64(m, op) || mach_is_f32(m, op);
}

/*
 * R10 is reserved scratch, but adjacent SOR-style floating operations do not
 * clobber it. Preserve a cached indexed-load induction value across only
 * those instructions; every other operation invalidates the cache
 * conservatively.
 */
static bool mach_inst_preserves_r10_index(const nyr_x64_mach_enc_t *e,
                                          const nyr_mach_inst_t *in) {
  if (in->opcode == NYR_MACH_LEA && in->dst.kind == NYR_MACH_OPERAND_VREG &&
      mach_sib_lea_mem(e, in->dst.as.reg, e->cur_inst + 1, NULL, NULL, NULL,
                       NULL))
    return true;
  if (in->opcode == NYR_MACH_ADD && in->dst.kind == NYR_MACH_OPERAND_VREG &&
      mach_sib_add_mem(e, in->dst.as.reg, e->cur_inst + 1, NULL, NULL, NULL,
                       NULL))
    return true;
  if (!mach_is_float(e->mach, &in->dst))
    return false;
  return in->opcode == NYR_MACH_COPY || in->opcode == NYR_MACH_LOAD ||
         in->opcode == NYR_MACH_ADD || in->opcode == NYR_MACH_SUB ||
         in->opcode == NYR_MACH_MUL || in->opcode == NYR_MACH_DIV;
}

static bool mach_is_v128(const nyr_mach_func_t *m,
                         const nyr_mach_operand_t *op) {
  if (!op || op->kind != NYR_MACH_OPERAND_VREG || op->as.reg >= m->vreg_len ||
      !m->vreg_types)
    return false;
  nyr_mach_type_t t = m->vreg_types[op->as.reg];
  return t == NYR_MACH_TYPE_V128_I64 || t == NYR_MACH_TYPE_V128_F64 ||
         t == NYR_MACH_TYPE_V128_F32 || t == NYR_MACH_TYPE_V256_I64 ||
         t == NYR_MACH_TYPE_V256_F64;
}

/*
 * A 32-byte vector vreg regardless of element kind (ymm homes and moves).
 */
static bool mach_vreg_is_v256(const nyr_mach_func_t *mach, uint32_t vreg) {
  return mach && vreg < mach->vreg_len && mach->vreg_types &&
         (mach->vreg_types[vreg] == NYR_MACH_TYPE_V256_I64 ||
          mach->vreg_types[vreg] == NYR_MACH_TYPE_V256_F64);
}
static bool mach_operand_is_preg(const nyr_mach_operand_t *op) {
  return op && op->kind == NYR_MACH_OPERAND_PREG;
}

static bool mach_inst_has_preg(const nyr_mach_inst_t *in) {
  return in &&
         (mach_operand_is_preg(&in->dst) || mach_operand_is_preg(&in->src0) ||
          mach_operand_is_preg(&in->src1) || mach_operand_is_preg(&in->src2));
}

static bool mach_inst_uses_vreg(const nyr_mach_inst_t *in, uint32_t vreg) {
  if (!in)
    return false;
  const nyr_mach_operand_t *ops[] = {&in->dst, &in->src0, &in->src1, &in->src2};
  for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); ++i)
    if (ops[i]->kind == NYR_MACH_OPERAND_VREG && ops[i]->as.reg == vreg)
      return true;
  for (size_t i = 0; i < in->args_len; ++i)
    if (in->args[i].kind == NYR_MACH_OPERAND_VREG && in->args[i].as.reg == vreg)
      return true;
  return false;
}

static size_t mach_inst_input_uses_vreg(const nyr_mach_inst_t *in,
                                        uint32_t vreg) {
  if (!in)
    return 0;
  const nyr_mach_operand_t *ops[] = {&in->src0, &in->src1, &in->src2};
  size_t uses = 0;
  for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); ++i)
    uses += ops[i]->kind == NYR_MACH_OPERAND_VREG && ops[i]->as.reg == vreg;
  for (size_t i = 0; i < in->args_len; ++i)
    uses +=
        in->args[i].kind == NYR_MACH_OPERAND_VREG && in->args[i].as.reg == vreg;
  return uses;
}

/*
 * A scalar load used only as the RHS of the immediately following integer
 * compare can stay in memory.  The load and compare must be adjacent in one
 * block, and no other consumer may observe the loaded value.
 */
static bool mach_operand_const_i64(const nyr_x64_mach_enc_t *e,
                                   const nyr_mach_operand_t *op,
                                   int64_t *value) {
  if (!e || !e->mach || !op || !value)
    return false;
  if (op->kind == NYR_MACH_OPERAND_IMM) {
    *value = op->as.imm;
    return true;
  }
  if (op->kind == NYR_MACH_OPERAND_VREG && op->as.reg < e->mach->vreg_len &&
      e->imm_fold_known && e->imm_fold_known[op->as.reg]) {
    *value = e->imm_fold[op->as.reg];
    return true;
  }
  return false;
}

/*
 * LLVM's x86 selector turns `(x & 1) == 0 ? a : b` into `test x, 1; cmovcc`.
 * Machine lowering currently materializes both the AND and boolean compare,
 * then reloads that boolean for SELECT. Mark only single-use, same-block
 * trees so the encoder can defer the test until after the select arms are
 * ready.
 */
static bool mach_prepare_select_bit_test(nyr_x64_mach_enc_t *e,
                                         const char *name) {
  if (!e || !e->mach || !e->select_test_src || !e->select_test_equal ||
      !e->cmp_elision_action || !e->select_skip_and || !e->def_inst)
    return false;
  const nyr_mach_func_t *m = e->mach;
#define SELECT_FUSE_TRACE(stage)                                               \
  do {                                                                         \
    if (ny_dbg_is_enabled(NY_DBG_DEEP)) {                                      \
      ny_dbg_enter_context(name);                                              \
      ny_dbg_heuristic_event("X64_SELECT_FUSE",                                \
                             "select=%zu cond=v%u stage=%s", si, cond, stage); \
      ny_dbg_exit_context(name);                                               \
    }                                                                          \
  } while (0)
  for (size_t si = 0; si < m->inst_len; ++si) {
    const nyr_mach_inst_t *select = &m->insts[si];
    if (select->opcode != NYR_MACH_SELECT ||
        select->src0.kind != NYR_MACH_OPERAND_VREG ||
        select->src0.as.reg >= m->vreg_len ||
        (select->condition != NYR_MACH_COND_EQ &&
         select->condition != NYR_MACH_COND_NE))
      continue;
    uint32_t cond = select->src0.as.reg;
    size_t ci = e->def_inst[cond];
    SELECT_FUSE_TRACE("candidate");
    if (ci == SIZE_MAX || ci >= si ||
        mach_inst_input_uses_vreg(select, cond) != 1) {
      SELECT_FUSE_TRACE("condition-def-or-use-count");
      continue;
    }
    const nyr_mach_inst_t *cmp = &m->insts[ci];
    if (cmp->opcode != NYR_MACH_CMP ||
        (cmp->condition != NYR_MACH_COND_EQ &&
         cmp->condition != NYR_MACH_COND_NE) ||
        mach_block_for_inst(e, ci) != mach_block_for_inst(e, si)) {
      SELECT_FUSE_TRACE("condition-not-same-block-eq-ne-compare");
      continue;
    }
    int64_t cmp_imm = 1;
    if (cmp->src1.kind == NYR_MACH_OPERAND_IMM)
      cmp_imm = cmp->src1.as.imm;
    else if (cmp->src1.kind == NYR_MACH_OPERAND_VREG &&
             cmp->src1.as.reg < m->vreg_len && e->imm_fold_known &&
             e->imm_fold_known[cmp->src1.as.reg])
      cmp_imm = e->imm_fold[cmp->src1.as.reg];
    else {
      SELECT_FUSE_TRACE("compare-rhs-not-constant");
      continue;
    }
    if (cmp_imm != 0 || cmp->src0.kind != NYR_MACH_OPERAND_VREG ||
        cmp->src0.as.reg >= m->vreg_len) {
      SELECT_FUSE_TRACE("compare-not-zero-against-vreg");
      continue;
    }
    uint32_t and_v = cmp->src0.as.reg;
    size_t ai = e->def_inst[and_v];
    if (ai == SIZE_MAX || ai >= ci ||
        mach_inst_input_uses_vreg(cmp, and_v) != 1 ||
        mach_block_for_inst(e, ai) != mach_block_for_inst(e, si)) {
      SELECT_FUSE_TRACE("and-def-or-use-count");
      continue;
    }
    const nyr_mach_inst_t *and = &m->insts[ai];
    if (and->opcode != NYR_MACH_AND) {
      SELECT_FUSE_TRACE("condition-source-not-and");
      continue;
    }
    int64_t mask = 0;
    uint32_t source = UINT32_MAX;
    if (and->src0.kind == NYR_MACH_OPERAND_VREG &&
        mach_operand_const_i64(e, &and->src1, &mask)) {
      source = and->src0.as.reg;
    } else if (and->src1.kind == NYR_MACH_OPERAND_VREG &&
               mach_operand_const_i64(e, &and->src0, &mask)) {
      source = and->src1.as.reg;
    } else {
      SELECT_FUSE_TRACE("and-not-constant-mask");
      continue;
    }
    if (mask != 1 || source >= m->vreg_len ||
        mach_inst_input_uses_vreg(and, source) != 1) {
      SELECT_FUSE_TRACE("mask-not-one-or-source-use-count");
      continue;
    }
    size_t and_uses = 0, cmp_uses = 0;
    size_t and_user = SIZE_MAX, cmp_user = SIZE_MAX;
    for (size_t ui = 0; ui < m->inst_len; ++ui) {
      size_t au = mach_inst_input_uses_vreg(&m->insts[ui], and_v);
      size_t cu = mach_inst_input_uses_vreg(&m->insts[ui], cond);
      if (au) {
        and_uses += au;
        and_user = ui;
      }
      if (cu) {
        cmp_uses += cu;
        cmp_user = ui;
      }
    }
    if (and_uses != 1 || and_user != ci || cmp_uses != 1 || cmp_user != si) {
      SELECT_FUSE_TRACE("tree-not-single-use");
      continue;
    }
    e->select_test_src[cond] = (int32_t)source;
    e->select_test_equal[cond] = (cmp->condition == NYR_MACH_COND_EQ) !=
                                 (select->condition == NYR_MACH_COND_EQ);
    e->cmp_elision_action[cond] = MACH_CMP_SKIP_FOR_SELECT;
    e->select_skip_and[and_v] = 1;
    SELECT_FUSE_TRACE("accepted");
  }
#undef SELECT_FUSE_TRACE
  return true;
}

/*
 * A missing home is safe only when the encoder consumes this literal as an
 * immediate. Other uses may read the canonical slot directly, so keep those
 * homes even though the value has a rematerialization fact.
 */
static bool mach_literal_rhs_use_is_immediate(const nyr_x64_mach_enc_t *e,
                                              const nyr_mach_inst_t *in,
                                              uint32_t vreg) {
  if (!e || !e->mach || !in || in->src1.kind != NYR_MACH_OPERAND_VREG ||
      in->src1.as.reg != vreg ||
      (in->src0.kind == NYR_MACH_OPERAND_VREG && in->src0.as.reg == vreg) ||
      (in->src2.kind == NYR_MACH_OPERAND_VREG && in->src2.as.reg == vreg))
    return false;
  int64_t value = e->imm_fold[vreg];
  switch (in->opcode) {
  case NYR_MACH_ADD:
  case NYR_MACH_SUB:
  case NYR_MACH_MUL:
  case NYR_MACH_AND:
  case NYR_MACH_OR:
  case NYR_MACH_XOR:
    return (int64_t)(int32_t)value == value;
  case NYR_MACH_DIV:
  case NYR_MACH_MOD:
    return value != 0 && value != 1 && value != -1;
  case NYR_MACH_SHL:
  case NYR_MACH_SAR:
  case NYR_MACH_ROR:
  case NYR_MACH_ROR32:
    return true;
  default:
    return false;
  }
}

static bool mach_build_literal_use_map(nyr_x64_mach_enc_t *e) {
  if (!e || !e->mach)
    return false;
  size_t vregs = e->mach->vreg_len;
  e->vreg_literal_rhs_only =
      calloc(vregs ? vregs : 1, sizeof(*e->vreg_literal_rhs_only));
  if (!e->vreg_literal_rhs_only)
    return false;
  for (size_t v = 0; v < vregs; ++v)
    e->vreg_literal_rhs_only[v] =
        e->imm_fold && e->imm_fold_known && e->imm_fold_known[v] &&
        e->mach->vreg_classes[v] == NYR_MACH_REGCLASS_GPR &&
        e->mach->vreg_types[v] == NYR_MACH_TYPE_I64;
  for (size_t i = 0; i < e->mach->inst_len; ++i) {
    const nyr_mach_inst_t *in = &e->mach->insts[i];
    const nyr_mach_operand_t *ops[] = {&in->src0, &in->src1, &in->src2};
    for (size_t oi = 0; oi < sizeof(ops) / sizeof(ops[0]); ++oi) {
      const nyr_mach_operand_t *op = ops[oi];
      if (op->kind == NYR_MACH_OPERAND_VREG && op->as.reg < vregs &&
          e->vreg_literal_rhs_only[op->as.reg] &&
          !mach_literal_rhs_use_is_immediate(e, in, op->as.reg))
        e->vreg_literal_rhs_only[op->as.reg] = false;
    }
    if (in->opcode == NYR_MACH_STORE && in->dst.kind == NYR_MACH_OPERAND_VREG &&
        in->dst.as.reg < vregs)
      e->vreg_literal_rhs_only[in->dst.as.reg] = false;
    for (size_t ai = 0; ai < in->args_len; ++ai) {
      const nyr_mach_operand_t *op = &in->args[ai];
      if (op->kind == NYR_MACH_OPERAND_VREG && op->as.reg < vregs)
        e->vreg_literal_rhs_only[op->as.reg] = false;
    }
  }
  return true;
}

static bool mach_build_vreg_block_use(nyr_x64_mach_enc_t *e) {
  if (!e || !e->mach || !e->mach->vreg_len || !e->mach->block_len)
    return true;
  const size_t vregs = e->mach->vreg_len;
  const size_t blocks = e->mach->block_len;
  e->vreg_has_ret_use = calloc(vregs, sizeof(*e->vreg_has_ret_use));
  e->vreg_last_call_arg_inst =
      ny_malloc_array(vregs, sizeof(*e->vreg_last_call_arg_inst));
  if (e->regalloc.segment_len)
    e->vreg_carried_copy_cache =
        calloc(e->regalloc.segment_len, sizeof(*e->vreg_carried_copy_cache));
  if (!e->vreg_has_ret_use || !e->vreg_last_call_arg_inst ||
      (e->regalloc.segment_len && !e->vreg_carried_copy_cache)) {
    free(e->vreg_has_ret_use);
    free(e->vreg_last_call_arg_inst);
    free(e->vreg_carried_copy_cache);
    e->vreg_has_ret_use = NULL;
    e->vreg_last_call_arg_inst = NULL;
    e->vreg_carried_copy_cache = NULL;
    return mach_err(e, "x64 machine form encode: OOM (vreg use map)");
  }
  for (size_t v = 0; v < vregs; ++v)
    e->vreg_last_call_arg_inst[v] = SIZE_MAX;

  for (size_t bi = 0; bi < blocks; ++bi) {
    const nyr_mach_block_t *block = &e->mach->blocks[bi];
    size_t first = block->first_inst;
    size_t end = first;
    if (block->inst_count > SIZE_MAX - first)
      end = e->mach->inst_len;
    else
      end += block->inst_count;
    if (first > e->mach->inst_len)
      first = e->mach->inst_len;
    if (end > e->mach->inst_len)
      end = e->mach->inst_len;
    for (size_t ii = first; ii < end; ++ii) {
      const nyr_mach_inst_t *in = &e->mach->insts[ii];
      if (in->opcode == NYR_MACH_RET &&
          in->src0.kind == NYR_MACH_OPERAND_VREG && in->src0.as.reg < vregs)
        e->vreg_has_ret_use[in->src0.as.reg] = true;
      for (size_t oi = 0; oi < in->args_len; ++oi) {
        const nyr_mach_operand_t *op = &in->args[oi];
        if (op->kind != NYR_MACH_OPERAND_VREG || op->as.reg >= vregs)
          continue;
        if (in->opcode == NYR_MACH_CALL)
          e->vreg_last_call_arg_inst[op->as.reg] = ii;
      }
    }
  }
  return true;
}

/*
 * Only operations whose x86 encodings preserve RFLAGS may sit between CMP
 * and its consuming BR_IF.  Fixed-register operands are also a boundary:
 * target-specific implicit register use is not represented in machine IR, so
 * keeping flags live across one would make the fusion proof incomplete.
 */
static bool mach_inst_preserves_flags(const nyr_mach_inst_t *in) {
  if (!in || mach_inst_has_preg(in))
    return false;
  switch (in->opcode) {
  case NYR_MACH_NOP:
  case NYR_MACH_COPY:
  case NYR_MACH_CONVERT:
  case NYR_MACH_LOAD:
  case NYR_MACH_STORE:
  case NYR_MACH_LEA:
  case NYR_MACH_FMA:
  case NYR_MACH_SQRT:
  case NYR_MACH_SIN:
  case NYR_MACH_COS:
  case NYR_MACH_SHUFFLE:
    /*
     * NOTE: NYR_MACH_REDUCE_ADD is NOT here — the i64 variant ends with
     * `add %rdx,%rax`, which writes FLAGS. Fusing a CMP across it would
     * branch on the add's flags.
     */
    return true;
  default:
    return false;
  }
}

static bool mach_cmp_branch_fusable(
    const nyr_x64_mach_enc_t *e, const nyr_mach_block_t *blk, size_t cmp_idx,
    const nyr_mach_inst_t *cmp, bool require_allocation, size_t *branch_idx_out,
    const char **reason_out, size_t *barrier_idx_out,
    nyr_mach_opcode_t *barrier_opcode_out) {
#define REJECT(s)                                                              \
  do {                                                                         \
    if (reason_out)                                                            \
      *reason_out = (s);                                                       \
    return false;                                                              \
  } while (0)
  if (barrier_idx_out)
    *barrier_idx_out = SIZE_MAX;
  if (!e || !e->mach || !blk || !cmp ||
      cmp->dst.kind != NYR_MACH_OPERAND_VREG || mach_inst_has_preg(cmp))
    REJECT("invalid-compare");
  size_t end = blk->first_inst + blk->inst_count;
  size_t branch_idx = cmp_idx + 1;
  while (branch_idx < end &&
         mach_inst_preserves_flags(&e->mach->insts[branch_idx])) {
    if (mach_inst_uses_vreg(&e->mach->insts[branch_idx], cmp->dst.as.reg))
      REJECT("intermediate-use");
    ++branch_idx;
  }
  if (branch_idx >= end)
    REJECT("no-branch-in-block");
  const nyr_mach_inst_t *branch = &e->mach->insts[branch_idx];
  if (branch->opcode != NYR_MACH_BR_IF) {
    if (barrier_idx_out)
      *barrier_idx_out = branch_idx;
    if (barrier_opcode_out)
      *barrier_opcode_out = branch->opcode;
    REJECT("barrier-before-branch");
  }
  if (mach_inst_has_preg(branch))
    REJECT("branch-fixed-register");
  if (branch->src0.kind != NYR_MACH_OPERAND_VREG ||
      branch->src0.as.reg != cmp->dst.as.reg)
    REJECT("condition-mismatch");
  for (size_t i = cmp_idx + 1; i < branch_idx; ++i)
    if (mach_inst_uses_vreg(&e->mach->insts[i], cmp->dst.as.reg))
      REJECT("intermediate-use");
  if (require_allocation) {
    const nyr_mach_live_segment_t *seg =
        mach_segment_at(e, cmp->dst.as.reg, cmp_idx);
    if (!seg)
      REJECT("missing-live-segment");
    if (seg->end != branch_idx)
      REJECT("segment-not-ending-at-branch");
    if (nyr_mach_regalloc_live_out(
            &e->regalloc, (size_t)(blk - e->mach->blocks), cmp->dst.as.reg))
      REJECT("condition-live-out");
  } else {
    /*
     * Before register allocation, prove the compare result has no use except
     * this branch. That is enough to omit the materialized condition and the
     * repeated comparison's operands from allocator liveness.
     */
    for (size_t i = 0; i < e->mach->inst_len; ++i)
      if (i != branch_idx &&
          mach_inst_input_uses_vreg(&e->mach->insts[i], cmp->dst.as.reg))
        REJECT("condition-has-other-use");
  }
  if (branch_idx_out)
    *branch_idx_out = branch_idx;
  if (reason_out)
    *reason_out = "fused";
  return true;
#undef REJECT
}

static bool mach_block_has_edge(const nyr_mach_func_t *m, size_t src,
                                size_t dst) {
  if (!m || src >= m->block_len || dst >= m->block_len)
    return false;
  const nyr_mach_block_t *block = &m->blocks[src];
  if (block->first_inst > m->inst_len ||
      block->inst_count > m->inst_len - block->first_inst)
    return false;
  size_t end = block->first_inst + block->inst_count;
  if (end == block->first_inst)
    return src + 1 == dst;
  const nyr_mach_inst_t *last = &m->insts[end - 1];
  if (last->opcode == NYR_MACH_BR)
    return last->src1.kind == NYR_MACH_OPERAND_BLOCK &&
           last->src1.as.block_index == dst;
  if (last->opcode == NYR_MACH_BR_IF) {
    if (last->src1.kind == NYR_MACH_OPERAND_BLOCK &&
        last->src1.as.block_index == dst)
      return true;
    return src + 1 == dst;
  }
  return last->opcode != NYR_MACH_RET && last->opcode != NYR_MACH_TRAP &&
         src + 1 == dst;
}

static bool mach_block_has_only_pred(const nyr_mach_func_t *m, size_t block,
                                     size_t pred) {
  size_t count = 0;
  for (size_t src = 0; src < m->block_len; ++src) {
    if (!mach_block_has_edge(m, src, block))
      continue;
    if (src != pred || ++count > 1)
      return false;
  }
  return count == 1;
}

/*
 * Reuse integer compare flags only over a uniquely-selected false edge:
 * CMP/BR_IF -> false-fallthrough trampoline -> target CMP/BR_IF. This narrow
 * shape proves that the first comparison is the last flag writer on the
 * second compare's execution path.
 */
static bool mach_prepare_cmp_flag_reuse(nyr_x64_mach_enc_t *e,
                                        const char *name) {
  if (!e || !e->mach || !e->mach->vreg_len)
    return true;
  const nyr_mach_func_t *m = e->mach;
  size_t edge_shapes = 0;
  size_t accepted = 0;
  if (!e->cmp_elision_action)
    return false;
  bool trace_reuse = false;
  const char *trace_filter = getenv("NY_TRACE_X64_CMP_FUSE_FILTER");
  if (ny_dbg_verbosity() >= 6 && name && ny_dbg_function_selected(name) &&
      trace_filter && *trace_filter && strstr(name, trace_filter))
    trace_reuse = true;
  for (size_t src = 0; src + 2 < m->block_len; ++src) {
    const nyr_mach_block_t *first = &m->blocks[src];
    const nyr_mach_block_t *trampoline = &m->blocks[src + 1];
    if (first->first_inst > m->inst_len ||
        first->inst_count > m->inst_len - first->first_inst ||
        first->inst_count < 2 || trampoline->first_inst > m->inst_len ||
        trampoline->inst_count > m->inst_len - trampoline->first_inst ||
        trampoline->inst_count == 0)
      continue;
    size_t first_end = first->first_inst + first->inst_count;
    size_t trampoline_end = trampoline->first_inst + trampoline->inst_count;
    const nyr_mach_inst_t *first_branch = &m->insts[first_end - 1];
    size_t first_cmp_idx = first_end - 1;
    while (first_cmp_idx > first->first_inst &&
           mach_inst_preserves_flags(&m->insts[first_cmp_idx - 1]))
      --first_cmp_idx;
    if (first_cmp_idx == first->first_inst)
      continue;
    --first_cmp_idx;
    const nyr_mach_inst_t *first_cmp = &m->insts[first_cmp_idx];
    const nyr_mach_inst_t *jump = &m->insts[trampoline_end - 1];
    bool trampoline_preserves_flags = true;
    for (size_t i = trampoline->first_inst; i + 1 < trampoline_end; ++i)
      if (!mach_inst_preserves_flags(&m->insts[i])) {
        trampoline_preserves_flags = false;
        break;
      }
    if (first_cmp->opcode != NYR_MACH_CMP ||
        first_branch->opcode != NYR_MACH_BR_IF || jump->opcode != NYR_MACH_BR ||
        !trampoline_preserves_flags ||
        jump->src1.kind != NYR_MACH_OPERAND_BLOCK ||
        first_branch->src1.kind != NYR_MACH_OPERAND_BLOCK ||
        first_branch->src1.as.block_index == src + 1 ||
        !mach_block_has_edge(m, src, src + 1))
      continue;
    ++edge_shapes;
    size_t target = jump->src1.as.block_index;
    if (target >= m->block_len || target == src || target == src + 1 ||
        !mach_block_has_only_pred(m, src + 1, src) ||
        !mach_block_has_only_pred(m, target, src + 1))
      continue;
    if (!mach_cmp_branch_fusable(e, first, first_cmp_idx, first_cmp, false,
                                 NULL, NULL, NULL, NULL))
      continue;
    const nyr_mach_block_t *second = &m->blocks[target];
    if (second->first_inst > m->inst_len ||
        second->inst_count > m->inst_len - second->first_inst)
      continue;
    size_t cmp_idx = second->first_inst;
    size_t second_end = second->first_inst + second->inst_count;
    while (cmp_idx < second_end &&
           mach_inst_preserves_flags(&m->insts[cmp_idx]))
      ++cmp_idx;
    if (cmp_idx + 1 >= second_end)
      continue;
    const nyr_mach_inst_t *second_cmp = &m->insts[cmp_idx];
    if (second_cmp->opcode != NYR_MACH_CMP ||
        second_cmp->dst.kind != NYR_MACH_OPERAND_VREG ||
        second_cmp->dst.as.reg >= m->vreg_len ||
        mach_is_float(m, &second_cmp->src0) ||
        first_cmp->src0.kind != NYR_MACH_OPERAND_VREG ||
        first_cmp->src1.kind != NYR_MACH_OPERAND_VREG ||
        second_cmp->src0.kind != NYR_MACH_OPERAND_VREG ||
        second_cmp->src1.kind != NYR_MACH_OPERAND_VREG ||
        first_cmp->src0.as.reg != second_cmp->src0.as.reg ||
        first_cmp->src1.as.reg != second_cmp->src1.as.reg ||
        !mach_cmp_branch_fusable(e, second, cmp_idx, second_cmp, false, NULL,
                                 NULL, NULL, NULL))
      continue;
    uint32_t cmp_reg = second_cmp->dst.as.reg;
    if (e->cmp_elision_action[cmp_reg] == MACH_CMP_KEEP) {
      e->cmp_elision_action[cmp_reg] = MACH_CMP_REUSE_FLAGS;
      ++accepted;
    }
  }
  if (trace_reuse) {
    ny_dbg_enter_context(name);
    ny_dbg_heuristic_event("X64_CMP_REUSE",
                           "blocks=%zu false-edge-shapes=%zu accepted=%zu",
                           m->block_len, edge_shapes, accepted);
    ny_dbg_exit_context(name);
  }
  return true;
}

/*
 * Build a temporary liveness view for reused compares. Emission still consumes
 * the original machine IR; the view only removes operands that the proven
 * flags-reuse path does not materialize or read.
 */
static bool mach_cmp_reuse_alloc_view(nyr_x64_mach_enc_t *e,
                                      nyr_mach_func_t *view,
                                      nyr_mach_inst_t **inst_copy_out) {
  if (!e || !e->mach || !view || !inst_copy_out)
    return false;
  *inst_copy_out = NULL;
  const nyr_mach_func_t *m = e->mach;
  bool has_reuse = false;
  for (size_t v = 0; v < m->vreg_len; ++v)
    has_reuse |= e->cmp_elision_action[v] == MACH_CMP_REUSE_FLAGS;
  if (!has_reuse)
    return true;
  if (m->inst_len > SIZE_MAX / sizeof(**inst_copy_out))
    return false;
  nyr_mach_inst_t *inst_copy = malloc(m->inst_len * sizeof(*inst_copy));
  if (!inst_copy)
    return false;
  memcpy(inst_copy, m->insts, m->inst_len * sizeof(*inst_copy));
  *view = *m;
  view->insts = inst_copy;
  for (size_t i = 0; i < m->inst_len; ++i) {
    const nyr_mach_inst_t *in = &m->insts[i];
    if (in->opcode != NYR_MACH_CMP || in->dst.kind != NYR_MACH_OPERAND_VREG ||
        in->dst.as.reg >= m->vreg_len ||
        e->cmp_elision_action[in->dst.as.reg] != MACH_CMP_REUSE_FLAGS)
      continue;
    size_t bi = 0;
    while (bi < m->block_len &&
           !(i >= m->blocks[bi].first_inst &&
             i - m->blocks[bi].first_inst < m->blocks[bi].inst_count))
      ++bi;
    size_t branch_idx = 0;
    if (bi == m->block_len ||
        !mach_cmp_branch_fusable(e, &m->blocks[bi], i, in, false, &branch_idx,
                                 NULL, NULL, NULL)) {
      free(inst_copy);
      *inst_copy_out = NULL;
      return false;
    }
    inst_copy[i].dst = (nyr_mach_operand_t){0};
    inst_copy[i].src0 = (nyr_mach_operand_t){0};
    inst_copy[i].src1 = (nyr_mach_operand_t){0};
    inst_copy[branch_idx].src0 = (nyr_mach_operand_t){0};
  }
  *inst_copy_out = inst_copy;
  return true;
}

static nyr_mach_type_t mach_v128_type(const nyr_mach_func_t *m,
                                      const nyr_mach_operand_t *op) {
  if (!mach_is_v128(m, op))
    return NYR_MACH_TYPE_NONE;
  return m->vreg_types[op->as.reg];
}

static bool mach_op_is_vreg(const nyr_mach_operand_t *op, uint32_t vreg) {
  return op && op->kind == NYR_MACH_OPERAND_VREG && op->as.reg == vreg;
}

static bool mach_use_requires_home(const nyr_mach_func_t *m,
                                   const nyr_mach_inst_t *in, uint32_t vreg) {
  if (!m || !in)
    return false;
  if (mach_op_is_vreg(&in->src1, vreg) || mach_op_is_vreg(&in->src2, vreg))
    return true;
  if (mach_op_is_vreg(&in->dst, vreg) && in->opcode == NYR_MACH_STORE) {
    /*
     * STORE.dst is the destination address, not the stored value. Keep a
     * home when scalar integer lowering must load the value through RAX
     * before resolving an uncolored address; float and narrow-immediate
     * stores resolve the address first and can consume the transient RAX
     * value directly.
     */
    if (mach_is_float(m, &in->src0))
      return false;
    if (in->src0.kind == NYR_MACH_OPERAND_IMM &&
        (int64_t)(int32_t)in->src0.as.imm == in->src0.as.imm)
      return false;
    return true;
  }
  if (mach_op_is_vreg(&in->src0, vreg)) {
    switch (in->opcode) {
    case NYR_MACH_COPY:
    case NYR_MACH_ADD:
    case NYR_MACH_SUB:
    case NYR_MACH_MUL:
    case NYR_MACH_AND:
    case NYR_MACH_OR:
    case NYR_MACH_XOR:
    case NYR_MACH_CMP:
      /*
       * These scalar consumers load src0 through the register/RAX-aware
       * materializer before any fixed-register barrier.
       */
      return false;
    case NYR_MACH_DIV:
    case NYR_MACH_MOD:
    case NYR_MACH_SQRT:
    case NYR_MACH_SIN:
    case NYR_MACH_COS:
    case NYR_MACH_SHL:
    case NYR_MACH_SAR:
    case NYR_MACH_ROR:
    case NYR_MACH_ROR32:
    case NYR_MACH_BSWAP64:
    case NYR_MACH_BSWAP32:
    case NYR_MACH_RET:
      /*
       * Return values are consumed after the producer's segment bookkeeping;
       * keep the canonical home populated for integer call results too.
       */
      return true;
    case NYR_MACH_LOAD:
      /*
       * src0 is the address operand; a dead value used only by this load can
       * stay in its existing register instead of being materialized in a
       * home.
       */
      return false;
    case NYR_MACH_STORE:
      /*
       * A frame store is an observable materialization of the source value.
       * Keep call results in their canonical home instead of relying on the
       * transient RAX forwarding cache across machine segments.
       */
      return true;
    default:
      return true;
    }
  }
  if (in->opcode == NYR_MACH_CALL && in->args) {
    for (size_t i = 0; i < in->args_len; ++i)
      if (mach_op_is_vreg(&in->args[i], vreg))
        return true;
  }
  return false;
}

/*
 * A selected scalar can stay only in its colored register when a later
 * same-block commutative ALU instruction has a colored destination and reads
 * that value as src1.  The native integer ALU path consumes both colors
 * directly, even though the generic home-use classifier conservatively marks
 * src1 as stack-backed.  Do not extend this proof across block boundaries:
 * loop exits and joins still use canonical homes.
 */
static bool mach_same_block_alu_src1_uses_color(const nyr_x64_mach_enc_t *e,
                                                const nyr_mach_inst_t *in,
                                                size_t use_idx, uint32_t vreg) {
  if (!e || !e->mach || !in || use_idx <= e->cur_inst ||
      !mach_op_is_vreg(&in->src1, vreg) ||
      in->dst.kind != NYR_MACH_OPERAND_VREG ||
      in->src0.kind != NYR_MACH_OPERAND_VREG)
    return false;
  size_t def_block = mach_block_for_inst(e, e->cur_inst);
  size_t use_block = mach_block_for_inst(e, use_idx);
  if (def_block == SIZE_MAX || def_block != use_block)
    return false;
  bool commutative_alu =
      in->opcode == NYR_MACH_ADD || in->opcode == NYR_MACH_MUL ||
      in->opcode == NYR_MACH_AND || in->opcode == NYR_MACH_OR ||
      in->opcode == NYR_MACH_XOR;
  if (!commutative_alu || !mach_is_i64(e->mach, &in->dst) ||
      !mach_is_i64(e->mach, &in->src0) || !mach_is_i64(e->mach, &in->src1))
    return false;
  const nyr_mach_live_segment_t *source_now =
      mach_segment_at(e, vreg, e->cur_inst);
  const nyr_mach_live_segment_t *source_use = mach_segment_at(e, vreg, use_idx);
  const nyr_mach_live_segment_t *dest_use =
      mach_segment_at(e, in->dst.as.reg, use_idx);
  if (!source_now || !source_use || !dest_use || source_now->spill ||
      source_use->spill || source_now->color < 0 || dest_use->color < 0 ||
      source_now->color != source_use->color ||
      source_now->color == dest_use->color)
    return false;
  for (size_t j = e->cur_inst + 1; j < use_idx; ++j)
    if (e->mach->insts[j].opcode == NYR_MACH_CALL)
      return false;
  return true;
}

/*
 * Some machine calls keep their first argument in src0 while the remaining
 * arguments live in `args`.  Values used by those calls must have a canonical
 * home even when their defining segment and call share one basic block.
 */
static bool
mach_vreg_same_block_future_use_requires_home(const nyr_x64_mach_enc_t *e,
                                              uint32_t vreg) {
  if (!e || !e->mach)
    return true;
  if (mach_vreg_can_feed_from_color(e, vreg))
    return false;
  size_t block = mach_block_for_inst(e, e->cur_inst);
  if (block == SIZE_MAX || block >= e->mach->block_len)
    return true;
  const nyr_mach_block_t *b = &e->mach->blocks[block];
  size_t end = b->first_inst + b->inst_count;
  if (end > e->mach->inst_len)
    end = e->mach->inst_len;
  for (size_t i = e->cur_inst + 1; i < end; ++i) {
    const nyr_mach_inst_t *in = &e->mach->insts[i];
    if (!mach_inst_uses_vreg(in, vreg) ||
        !mach_use_requires_home(e->mach, in, vreg))
      continue;
    if (mach_same_block_alu_src1_uses_color(e, in, i, vreg))
      continue;
    return true;
  }
  return false;
}

/*
 * A spilled i64 result may skip its canonical home when a nearby consumer
 * consumes it in a home-free operand position and no later instruction
 * refers to the vreg. Pure V256 arithmetic between the definition and use
 * leaves RAX untouched; the encoder carries its sticky slot tag across those
 * instructions.
 */
static bool mach_vreg_can_forward_to_next_rax(const nyr_x64_mach_enc_t *e,
                                              uint32_t vreg) {
  if (!e || !e->mach || e->cur_inst + 1 >= e->mach->inst_len)
    return false;
  size_t block = mach_block_for_inst(e, e->cur_inst);
  if (block == SIZE_MAX)
    return false;
  const nyr_mach_block_t *b = &e->mach->blocks[block];
  /*
   * Forwarding suppresses the canonical stack-home write. That is only
   * legal for a value that dies in this block: a backedge use may appear
   * earlier in linear instruction order and would otherwise be invisible to
   * the forward scan below.
   */
  if (nyr_mach_regalloc_live_out(&e->regalloc, block, vreg))
    return false;
  size_t next_index = e->cur_inst + 1;
  size_t block_end = b->first_inst + b->inst_count;
  if (next_index >= block_end)
    return false;
  const nyr_mach_inst_t *next = &e->mach->insts[next_index];
  /*
   * An address ADD immediately feeding a scalar memory access can be
   * rematerialized as a SIB operand. That lowering loads the index into RAX
   * before loading the base, so a forwarded src0/base value would be
   * clobbered before it is consumed. Preserve its home for that case.
   */
  if (next->opcode == NYR_MACH_ADD && next->dst.kind == NYR_MACH_OPERAND_VREG &&
      next_index + 1 < b->first_inst + b->inst_count) {
    const nyr_mach_inst_t *access = &e->mach->insts[next_index + 1];
    if (access->opcode == NYR_MACH_LOAD || access->opcode == NYR_MACH_STORE) {
      const nyr_mach_operand_t *base = NULL;
      if (mach_sib_add_mem(e, next->dst.as.reg, next_index + 1, &base, NULL,
                           NULL, NULL) &&
          mach_op_is_vreg(base, vreg))
        return false;
    }
  }
  for (size_t i = next_index; i < block_end; ++i) {
    const nyr_mach_inst_t *use = &e->mach->insts[i];
    if (mach_inst_uses_vreg(use, vreg)) {
      bool direct_source = mach_op_is_vreg(&use->src0, vreg);
      /* The LEA encoder consumes a register index from RAX first, copies it
       * to the reserved index register, and only then loads the base. */
      bool direct_lea_index = use->opcode == NYR_MACH_LEA &&
                              mach_op_is_vreg(&use->src1, vreg) &&
                              use->src0.kind == NYR_MACH_OPERAND_VREG &&
                              use->src2.kind == NYR_MACH_OPERAND_IMM &&
                              (use->src2.as.imm == 2 || use->src2.as.imm == 4 ||
                               use->src2.as.imm == 8);
      /*
       * A LEA feeding a memory access can be folded back into that access.
       * The folded encoder may load the base after the LEA index; do not
       * forward the index only in RAX because that would leave no canonical
       * home to reload after the base load clobbers RAX.
       */
      if (direct_lea_index && i + 1 < block_end &&
          use->dst.kind == NYR_MACH_OPERAND_VREG) {
        const nyr_mach_inst_t *access = &e->mach->insts[i + 1];
        bool uses_address = (access->opcode == NYR_MACH_LOAD &&
                             mach_op_is_vreg(&access->src0, use->dst.as.reg)) ||
                            (access->opcode == NYR_MACH_STORE &&
                             mach_op_is_vreg(&access->dst, use->dst.as.reg));
        if (uses_address &&
            mach_sib_lea_mem(e, use->dst.as.reg, i + 1, NULL, NULL, NULL, NULL))
          return false;
      }
      bool store_address =
          use->opcode == NYR_MACH_STORE && mach_op_is_vreg(&use->dst, vreg) &&
          (mach_is_float(e->mach, &use->src0) ||
           (use->src0.kind == NYR_MACH_OPERAND_IMM &&
            (int64_t)(int32_t)use->src0.as.imm == use->src0.as.imm));
      if ((!direct_source && !direct_lea_index && !store_address) ||
          (mach_use_requires_home(e->mach, use, vreg) && !direct_lea_index))
        return false;
      for (size_t later = i + 1; later < block_end; ++later)
        if (mach_inst_uses_vreg(&e->mach->insts[later], vreg))
          return false;
      return true;
    }
    /* V256 ALU instructions use vector registers only, preserving RAX and
     * allowing its cached value to reach the later scalar LEA index use. */
    bool vector_alu =
        use->dst.kind == NYR_MACH_OPERAND_VREG &&
        mach_vreg_is_v256(e->mach, use->dst.as.reg) &&
        (use->opcode == NYR_MACH_ADD || use->opcode == NYR_MACH_SUB ||
         use->opcode == NYR_MACH_MUL || use->opcode == NYR_MACH_AND ||
         use->opcode == NYR_MACH_OR || use->opcode == NYR_MACH_XOR);
    if (!vector_alu)
      return false;
  }
  return false;
}

/*
 * A carried phi intentionally has no home write on its backedge.  When the
 * carried value later leaves the loop and is consumed by an ordinary segment,
 * materialize its current preg once on that cold path before the consumer.
 */
static bool mach_sync_carried_consumers(nyr_x64_mach_enc_t *e,
                                        const nyr_mach_block_t *block) {
  if (!e || !e->mach || !block || !e->color_seeded)
    return true;
  for (uint32_t v = 0; v < e->colors_len; ++v) {
    /*
     * A carried segment elsewhere in the function is not evidence that this
     * block currently has the value in its physical register.  Synchronize
     * only an actually seeded resident value; otherwise a later segment can
     * cause an unrelated register to overwrite this vreg's canonical home.
     */
    if (!e->color_seeded[v])
      continue;
    const nyr_mach_live_segment_t *seg =
        mach_segment_at(e, v, block->first_inst);
    bool used = false;
    bool requires_home = false;
    for (size_t i = 0; i < block->inst_count; ++i) {
      const nyr_mach_inst_t *in = &e->mach->insts[block->first_inst + i];
      if (!mach_inst_uses_vreg(in, v))
        continue;
      used = true;
      requires_home = requires_home || mach_use_requires_home(e->mach, in, v);
    }
    if (!used)
      continue;
    /*
     * A loop-carried register is authoritative only for uses the machine
     * encoder can consume directly from that register. Return operands and
     * other home-only uses need the carried value published after leaving
     * the loop, even when their segment still bears the carried marker.
     */
    if (!seg || !seg->carried || seg->spill || seg->color < 0 ||
        seg->color >= MIR_COLOR_N)
      continue;
    if (!requires_home)
      continue;
    int preg = mach_color_preg[seg->color];
    if (!mach_store_preg_home(e, preg, mach_vreg_home_off(e, v)))
      return false;
    e->color_seeded[v] = false;
    mach_rax_invalidate(e);
  }
  return true;
}

/*
 * True when vreg has a use in a block after cur_inst's block.  Such uses
 * re-seed from the stack home at block entry (color_seeded is reset per
 * edge), so the home must be kept current even when the allocator keeps
 * the value in a colored register inside this block.
 */
static bool mach_vreg_used_in_later_block(const nyr_x64_mach_enc_t *e,
                                          uint32_t vreg) {
  if (!e || !e->mach)
    return true;
  size_t block = mach_block_for_inst(e, e->cur_inst);
  if (block == SIZE_MAX)
    return true;
  /*
   * Liveness is CFG-aware, unlike a linear scan.  In particular a value used
   * on a loop backedge can be live-out even though all textual uses precede
   * its definition.  Such a value needs a canonical home unless a separately
   * proven carried-register contract handles the edge.
   */
  if (nyr_mach_regalloc_live_out(&e->regalloc, block, vreg))
    return true;
  /*
   * Machine SSA has already made PHI edge values explicit before allocation.
   * The allocator's fixed-point CFG liveness therefore accounts for every
   * successor use, including loop backedges; textual mentions in another
   * block are not sufficient evidence that this block must home the value.
   */
  return false;
}

/*
 * A spilled colored scalar can feed one same-block scalar operation directly
 * from its assigned preg when that operation is its final use. Keep the home
 * write for CFG edges, calls, fixed-register instructions, and any gap that
 * could reuse the color before the value is consumed.
 */
static bool mach_vreg_can_feed_from_color(const nyr_x64_mach_enc_t *e,
                                          uint32_t vreg) {
  if (!e || !e->mach || vreg >= e->colors_len || e->force_stack_reads ||
      mach_vreg_used_in_later_block(e, vreg) || mach_vreg_has_ret_use(e, vreg))
    return false;
  nyr_mach_operand_t value = {.kind = NYR_MACH_OPERAND_VREG,
                              .reg_class = NYR_MACH_REGCLASS_GPR,
                              .as.reg = vreg};
  if (!mach_is_i64(e->mach, &value))
    return false;
  const nyr_mach_live_segment_t *seg = mach_segment_at(e, vreg, e->cur_inst);
  if (!seg || seg->color < 0 || seg->color >= MIR_COLOR_N)
    return false;
  size_t block = mach_block_for_inst(e, e->cur_inst);
  if (block == SIZE_MAX || block >= e->mach->block_len)
    return false;
  const nyr_mach_block_t *b = &e->mach->blocks[block];
  size_t block_end = b->first_inst + b->inst_count;
  size_t use_idx = seg->end;
  if (use_idx <= e->cur_inst || use_idx >= block_end ||
      use_idx >= e->mach->inst_len)
    return false;
  const nyr_mach_inst_t *use = &e->mach->insts[use_idx];
  switch (use->opcode) {
  case NYR_MACH_COPY:
  case NYR_MACH_ADD:
  case NYR_MACH_SUB:
  case NYR_MACH_MUL:
  case NYR_MACH_AND:
  case NYR_MACH_OR:
  case NYR_MACH_XOR:
  case NYR_MACH_CMP:
  case NYR_MACH_SELECT:
    break;
  default:
    return false;
  }
  if (!mach_inst_input_uses_vreg(use, vreg) ||
      mach_use_requires_home(e->mach, use, vreg) || mach_inst_has_preg(use))
    return false;
  const nyr_mach_live_segment_t *use_seg = mach_segment_at(e, vreg, use_idx);
  if (!use_seg || use_seg->color != seg->color)
    return false;

  for (size_t i = e->cur_inst + 1; i <= use_idx; ++i) {
    const nyr_mach_inst_t *in = &e->mach->insts[i];
    if (mach_inst_has_preg(in))
      return false;
    if (mach_inst_input_uses_vreg(in, vreg) != (i == use_idx ? 1u : 0u))
      return false;
    if (i == use_idx)
      continue;
    switch (in->opcode) {
    case NYR_MACH_COPY:
    case NYR_MACH_ADD:
    case NYR_MACH_SUB:
    case NYR_MACH_MUL:
    case NYR_MACH_AND:
    case NYR_MACH_OR:
    case NYR_MACH_XOR:
    case NYR_MACH_CMP:
      break;
    default:
      return false;
    }
    if (in->dst.kind == NYR_MACH_OPERAND_VREG) {
      const nyr_mach_live_segment_t *dst_seg =
          mach_segment_at(e, in->dst.as.reg, i);
      if (dst_seg && dst_seg->color == seg->color)
        return false;
    }
  }
  return true;
}

static bool mach_vreg_future_use_requires_home(const nyr_x64_mach_enc_t *e,
                                               uint32_t vreg) {
  if (!e || !e->mach)
    return true;
  /*
   * Call lowering reloads uncolored operands from canonical homes. Keep this
   * ABI use visible even when the allocator's live segment ends before the
   * call because the call argument list is not a normal src operand.
   */
  if (e->vreg_last_call_arg_inst && vreg < e->mach->vreg_len) {
    size_t last_call = e->vreg_last_call_arg_inst[vreg];
    if (last_call != SIZE_MAX && last_call > e->cur_inst)
      return true;
  } else {
    for (size_t i = e->cur_inst + 1; i < e->mach->inst_len; ++i) {
      const nyr_mach_inst_t *in = &e->mach->insts[i];
      if (in->opcode != NYR_MACH_CALL || !in->args)
        continue;
      for (size_t a = 0; a < in->args_len; ++a)
        if (mach_op_is_vreg(&in->args[a], vreg))
          return true;
    }
  }
  /*
   * Literal homes can be omitted only when every use is immediate-folded.
   */
  if (vreg < e->mach->vreg_len && e->vreg_literal_rhs_only &&
      e->vreg_literal_rhs_only[vreg])
    return false;
  if (mach_vreg_can_feed_from_color(e, vreg))
    return false;
  const nyr_mach_live_segment_t *seg =

      mach_segment_at(e, vreg, e->cur_inst);
  if (!seg || seg->end <= e->cur_inst)
    return false;
  if (seg->spill)
    return true;
  size_t end = seg->end;
  if (end >= e->mach->inst_len)
    end = e->mach->inst_len - 1;
  for (size_t i = e->cur_inst + 1; i <= end; ++i) {
    const nyr_mach_inst_t *in = &e->mach->insts[i];
    if (!mach_inst_uses_vreg(in, vreg))
      continue;
    if (mach_use_requires_home(e->mach, in, vreg) &&
        !mach_same_block_alu_src1_uses_color(e, in, i, vreg))
      return true;
  }
  return false;
}

/*
 * movdqu xmmN <-> mem: F3 REX 0F 6F/7F 85 disp32 (unaligned).
 */
static bool mach_load_xmm_v128(nyr_x64_mach_enc_t *e, int off, unsigned reg) {
  if (reg >= 16)
    return mach_err(e, "x64 machine form encode: unsupported XMM register");
  return mach_u8(e, 0xf3) &&
         (reg >= 8 ? mach_rex(e, false, true, false, false) : true) &&
         mach_u8(e, 0x0f) && mach_u8(e, 0x6f) &&
         mach_u8(e, 0x85 | ((reg & 7) << 3)) && mach_i32(e, off);
}

static bool mach_store_xmm_v128(nyr_x64_mach_enc_t *e, int off, unsigned reg) {
  if (reg >= 16)
    return mach_err(e, "x64 machine form encode: unsupported XMM register");
  return mach_u8(e, 0xf3) &&
         (reg >= 8 ? mach_rex(e, false, true, false, false) : true) &&
         mach_u8(e, 0x0f) && mach_u8(e, 0x7f) &&
         mach_u8(e, 0x85 | ((reg & 7) << 3)) && mach_i32(e, off);
}

static bool mach_load_xmm0_v128(nyr_x64_mach_enc_t *e, int off) {
  return mach_load_xmm_v128(e, off, 0);
}

static bool mach_store_xmm0_v128(nyr_x64_mach_enc_t *e, int off) {
  return mach_store_xmm_v128(e, off, 0);
}

/*
 * ymm (256-bit VEX) helpers for V256_I64 fast path
 */

/*
 * vmovdqu ymmN, [rbp+off] — unaligned stack homes need no 32-byte rbp
 * alignment.
 */
static bool mach_ymm_load_v256(nyr_x64_mach_enc_t *e, int off, unsigned reg) {
  if (reg >= 16)
    return mach_err(e, "x64: unsupported YMM register");
  return mach_u8(e, 0xc4) && mach_u8(e, reg >= 8 ? 0x61 : 0xe1) &&
         mach_u8(e, 0x7e) && mach_u8(e, 0x6f) &&
         mach_u8(e, 0x85 | ((reg & 7) << 3)) && mach_i32(e, off);
}

/*
 * vmovdqu [rbp+off], ymmN — unaligned stack homes need no 32-byte rbp
 * alignment.
 */
static bool mach_ymm_store_v256(nyr_x64_mach_enc_t *e, int off, unsigned reg) {
  if (reg >= 16)
    return mach_err(e, "x64: unsupported YMM register");
  return mach_u8(e, 0xc4) && mach_u8(e, reg >= 8 ? 0x61 : 0xe1) &&
         mach_u8(e, 0x7e) && mach_u8(e, 0x7f) &&
         mach_u8(e, 0x85 | ((reg & 7) << 3)) && mach_i32(e, off);
}

/*
 * vmovdqa ymmN, [rax] — VEX.256.66.0F 6F /r (no disp)
 */
static bool mach_ymm_load_v256_ptr(nyr_x64_mach_enc_t *e, unsigned reg) {
  if (reg >= 16)
    return mach_err(e, "x64: unsupported YMM register");
  return mach_u8(e, 0xc4) && mach_u8(e, reg >= 8 ? 0x61 : 0xe1) &&
         mach_u8(e, 0x7e) && mach_u8(e, 0x6f) && mach_u8(e, (reg & 7) << 3);
}

/*
 * vmovdqa [rax], ymmN — VEX.256.66.0F 7F /r (no disp)
 */
static bool mach_ymm_store_v256_ptr(nyr_x64_mach_enc_t *e, unsigned reg) {
  if (reg >= 16)
    return mach_err(e, "x64: unsupported YMM register");
  return mach_u8(e, 0xc4) && mach_u8(e, reg >= 8 ? 0x61 : 0xe1) &&
         mach_u8(e, 0x7e) && mach_u8(e, 0x7f) && mach_u8(e, (reg & 7) << 3);
}

/*
 * vmovdqa ymmD, ymmS — VEX.256.66.0F 6F /r
 */
static bool mach_ymm_mov_reg(nyr_x64_mach_enc_t *e, unsigned dst,
                             unsigned src) {
  if (dst >= 16 || src >= 16)
    return mach_err(e, "x64: unsupported YMM register");
  unsigned char vex2 = (unsigned char)(0xe1u & ~(dst >= 8 ? 0x80u : 0u) &
                                       ~(src >= 8 ? 0x20u : 0u));
  return mach_u8(e, 0xc4) && mach_u8(e, vex2) && mach_u8(e, 0x7d) &&
         mach_u8(e, 0x6f) && mach_u8(e, 0xc0 | ((dst & 7) << 3) | (src & 7));
}

/*
 * vp* ymmD, ymmD, ymmS — VEX.256.66.0F.W1 opc /r
 */
static bool mach_ymm_op_reg(nyr_x64_mach_enc_t *e, unsigned dst, unsigned src,
                            unsigned opc) {
  if (dst >= 16 || src >= 16)
    return mach_err(e, "x64: unsupported YMM register");
  unsigned char vex2 = (unsigned char)(0xe1u & ~(dst >= 8 ? 0x80u : 0u) &
                                       ~(src >= 8 ? 0x20u : 0u));
  return mach_u8(e, 0xc4) && mach_u8(e, vex2) && mach_u8(e, 0x7d) &&
         mach_u8(e, opc) && mach_u8(e, 0xc0 | ((dst & 7) << 3) | (src & 7));
}

/*
 * vp* ymmD, ymmD, [rbp+off] — VEX.256.66.0F.W1 opc 85 disp32
 */
static bool mach_ymm_op_mem_reg(nyr_x64_mach_enc_t *e, unsigned dst, int off,
                                unsigned opc) {
  if (dst >= 16)
    return mach_err(e, "x64: unsupported YMM register");
  return mach_u8(e, 0xc4) && mach_u8(e, dst >= 8 ? 0x61 : 0xe1) &&
         mach_u8(e, 0x7d) && mach_u8(e, opc) &&
         mach_u8(e, 0x85 | ((dst & 7) << 3)) && mach_i32(e, off);
}

/*
 * vbroadcastsd ymmD, [rbp+off] — VEX.256.66.0F38.W1 19 /r. Four-lane f64
 * SET1: one memory broadcast replaces the scalar movddup used for V128.
 */
static bool mach_ymm_broadcastsd_mem(nyr_x64_mach_enc_t *e, unsigned reg,
                                     int off) {
  if (reg >= 16)
    return mach_err(e, "x64: unsupported YMM register");
  return mach_u8(e, 0xc4) && mach_u8(e, 0xe2) && mach_u8(e, 0x7d) &&
         mach_u8(e, 0x19) && mach_u8(e, 0x85 | ((reg & 7) << 3)) &&
         mach_i32(e, off);
}

/*
 * vfmadd213pd ymmD, ymmH, ymmS — VEX.NDS.256.66.0F38.W1 AA /r.
 * D = H * D + S; the 213 form accumulates in the first operand, so a seeded
 * destination register needs no extra move.
 */
static bool mach_ymm_vfmadd213_reg(nyr_x64_mach_enc_t *e, unsigned dst,
                                   unsigned mul, unsigned add) {
  if (dst >= 16 || mul >= 16 || add >= 16)
    return mach_err(e, "x64: unsupported YMM register");
  unsigned vex2 = 0xe2; /* ~R=1, ~X=1, ~B=1, map 0F38 */
  if (dst >= 8)
    vex2 &= ~0x80u;
  if (add >= 8)
    vex2 &= ~0x20u;
  unsigned char vex3 =
      (unsigned char)(0x80u | (((~mul) & 0xfu) << 3) | 0x04u | 0x01u);
  return mach_u8(e, 0xc4) && mach_u8(e, vex2) && mach_u8(e, vex3) &&
         mach_u8(e, 0xa8) && mach_u8(e, 0xc0u | ((dst & 7) << 3) | (add & 7));
}

/*
 * vfmadd213pd ymmD, ymmH, [rbp+off] — memory addend form.
 */
static bool mach_ymm_vfmadd213_mem(nyr_x64_mach_enc_t *e, unsigned dst,
                                   unsigned mul, int off) {
  if (dst >= 16 || mul >= 16)
    return mach_err(e, "x64: unsupported YMM register");
  unsigned vex2 = 0xe2;
  if (dst >= 8)
    vex2 &= ~0x80u;
  unsigned char vex3 =
      (unsigned char)(0x80u | (((~mul) & 0xfu) << 3) | 0x04u | 0x01u);
  return mach_u8(e, 0xc4) && mach_u8(e, vex2) && mach_u8(e, vex3) &&
         mach_u8(e, 0xa8) && mach_u8(e, 0x85u | ((dst & 7) << 3)) &&
         mach_i32(e, off);
}

/*
 * vextractf128 xmmD, ymmS, imm — VEX.256.66.0F3A.W0 19 /r ib. The reg
 * field carries the ymm source and rm the xmm destination.
 */
static bool mach_xmm_vextractf128(nyr_x64_mach_enc_t *e, unsigned dst,
                                  unsigned src, unsigned half) {
  if (dst >= 16 || src >= 16)
    return mach_err(e, "x64: unsupported vextractf128 register");
  unsigned vex2 = 0xe3; /* ~R=1, ~X=1, ~B=1, map 0F3A */
  if (src >= 8)
    vex2 &= ~0x80u;
  if (dst >= 8)
    vex2 &= ~0x20u;
  return mach_u8(e, 0xc4) && mach_u8(e, vex2) && mach_u8(e, 0x7d) &&
         mach_u8(e, 0x19) && mach_u8(e, 0xc0u | ((src & 7) << 3) | (dst & 7)) &&
         mach_u8(e, half);
}

/*
 * movddup xmmD, xmmS — F2 0F 12 /r: D = {S[0], S[0]}.
 */
static bool mach_xmm_movddup(nyr_x64_mach_enc_t *e, unsigned dst,
                             unsigned src) {
  if (dst >= 8 || src >= 8)
    return mach_err(e, "x64: unsupported movddup register");
  return mach_u8(e, 0xf2) && mach_u8(e, 0x0f) && mach_u8(e, 0x12) &&
         mach_u8(e, 0xc0u | (dst << 3) | src);
}

/*
 * movaps xmmD, xmmS — 0F 28 /r.
 */
static bool mach_xmm_movaps(nyr_x64_mach_enc_t *e, unsigned dst, unsigned src) {
  if (dst >= 8 || src >= 8)
    return mach_err(e, "x64: unsupported movaps register");
  return mach_u8(e, 0x0f) && mach_u8(e, 0x28) &&
         mach_u8(e, 0xc0u | (dst << 3) | src);
}

/*
 * shufpd xmmD, xmmS, imm — 66 0F C6 /r ib: D = {imm.0?S[0]:D[0],
 * imm.1?S[1]:D[1]}. Both registers must be below xmm8.
 */
static bool mach_xmm_shufpd(nyr_x64_mach_enc_t *e, unsigned dst, unsigned src,
                            unsigned imm) {
  if (dst >= 8 || src >= 8)
    return mach_err(e, "x64: unsupported shufpd register");
  return mach_u8(e, 0x66) && mach_u8(e, 0x0f) && mach_u8(e, 0xc6) &&
         mach_u8(e, 0xc0u | (dst << 3) | src) && mach_u8(e, imm);
}

/*
 * unpckhpd xmmD, xmmS — 66 0F 6A /r: D = {S[1], D[1]}. With D == S this is
 * the high-lane broadcast {S[1], S[1]}. Both registers must be below xmm8.
 */
static bool mach_xmm_unpckhpd(nyr_x64_mach_enc_t *e, unsigned dst,
                              unsigned src) {
  if (dst >= 8 || src >= 8)
    return mach_err(e, "x64: unsupported unpckhpd register");
  return mach_u8(e, 0x66) && mach_u8(e, 0x0f) && mach_u8(e, 0x15) &&
         mach_u8(e, 0xc0u | (dst << 3) | src);
}

/*
 * movhlps xmmD, xmmS — 0F 12 /r: D[0] <- S[1], D[1] unchanged. With
 * D == S this moves the high lane into the low slot.
 */
static bool mach_xmm_movhlps(nyr_x64_mach_enc_t *e, unsigned dst,
                             unsigned src) {
  if (dst >= 16 || src >= 16)
    return mach_err(e, "x64: unsupported movhlps register");
  unsigned rex = 0x40u | (dst >= 8 ? 0x04u : 0u) | (src >= 8 ? 0x01u : 0u);
  return mach_u8(e, rex) && mach_u8(e, 0x0f) && mach_u8(e, 0x12) &&
         mach_u8(e, 0xc0u | ((dst & 7) << 3) | (src & 7));
}

/*
 * movupd [rbp+off], xmmR — 66 0F 11 /r. Unaligned 16-byte half-home store.
 */
static bool mach_xmm_movupd_store_mem(nyr_x64_mach_enc_t *e, unsigned reg,
                                      int off) {
  if (reg >= 16)
    return mach_err(e, "x64: unsupported movupd register");
  return mach_u8(e, 0x66) &&
         (reg >= 8 ? mach_rex(e, false, true, false, false) : true) &&
         mach_u8(e, 0x0f) && mach_u8(e, 0x11) &&
         mach_u8(e, 0x85u | ((reg & 7) << 3)) && mach_i32(e, off);
}

static bool mach_load_xmm_v128_ptr(nyr_x64_mach_enc_t *e, unsigned reg) {
  if (reg >= 16)
    return mach_err(e, "x64 machine form encode: unsupported XMM register");
  return mach_u8(e, 0xf3) &&
         (reg >= 8 ? mach_rex(e, false, true, false, false) : true) &&
         mach_u8(e, 0x0f) && mach_u8(e, 0x6f) &&
         mach_u8(e, (unsigned)((reg & 7) << 3));
}

static bool mach_vec_mov_reg(nyr_x64_mach_enc_t *e, unsigned dst,
                             unsigned src) {
  if (dst >= 16 || src >= 16)
    return mach_err(e, "x64 machine form encode: unsupported XMM register");
  unsigned rex = 0x40 | (dst >= 8 ? 0x04 : 0) | (src >= 8 ? 0x01 : 0);
  return mach_u8(e, 0xf3) && (rex != 0x40 ? mach_u8(e, rex) : true) &&
         mach_u8(e, 0x0f) && mach_u8(e, 0x6f) &&
         mach_u8(e, 0xc0 | ((dst & 7) << 3) | (src & 7));
}

static bool mach_vec_op_reg(nyr_x64_mach_enc_t *e, unsigned dst, unsigned src,
                            unsigned prefix, unsigned opc) {
  if (dst >= 16 || src >= 16)
    return mach_err(e, "x64 machine form encode: unsupported XMM register");
  unsigned rex = 0x40 | (dst >= 8 ? 0x04 : 0) | (src >= 8 ? 0x01 : 0);
  return (prefix ? mach_u8(e, prefix) : true) &&
         (rex != 0x40 ? mach_u8(e, rex) : true) && mach_u8(e, 0x0f) &&
         mach_u8(e, opc) && mach_u8(e, 0xc0 | ((dst & 7) << 3) | (src & 7));
}

static bool mach_vec_op_mem_reg(nyr_x64_mach_enc_t *e, unsigned dst, int off,
                                unsigned prefix, unsigned opc) {
  if (dst >= 16)
    return mach_err(e, "x64 machine form encode: unsupported XMM register");
  /*
   * Vector spill homes are only 8-byte aligned. Legacy SSE packed arithmetic
   * memory operands require 16-byte alignment, so load the rhs with MOVDQU
   * into the reserved scratch XMM register and use the register form.
   */
  unsigned src = dst == 15 ? 14 : 15;
  return mach_load_xmm_v128(e, off, src) &&
         mach_vec_op_reg(e, dst, src, prefix, opc);
}

/*
 * movsd/movss xmmN <-> mem: F2/F3 REX 0F 10/11 modrm disp32.
 */
static bool mach_load_xmm(nyr_x64_mach_enc_t *e, int off, bool f32,
                          unsigned reg) {
  if (reg >= 16)
    return mach_err(e, "x64 machine form encode: unsupported XMM register");
  return mach_u8(e, f32 ? 0xf3 : 0xf2) &&
         (reg >= 8 ? mach_rex(e, false, true, false, false) : true) &&
         mach_u8(e, 0x0f) && mach_u8(e, 0x10) &&
         mach_u8(e, 0x85 | ((reg & 7) << 3)) && mach_i32(e, off);
}

static bool mach_store_xmm(nyr_x64_mach_enc_t *e, int off, bool f32,
                           unsigned reg) {
  if (reg >= 16)
    return mach_err(e, "x64 machine form encode: unsupported XMM register");
  return mach_u8(e, f32 ? 0xf3 : 0xf2) &&
         (reg >= 8 ? mach_rex(e, false, true, false, false) : true) &&
         mach_u8(e, 0x0f) && mach_u8(e, 0x11) &&
         mach_u8(e, 0x85 | ((reg & 7) << 3)) && mach_i32(e, off);
}

static bool mach_load_xmm0(nyr_x64_mach_enc_t *e, int off, bool f32) {
  return mach_load_xmm(e, off, f32, 0);
}

static bool mach_store_xmm0(nyr_x64_mach_enc_t *e, int off, bool f32) {
  return mach_store_xmm(e, off, f32, 0);
}

static bool mach_load_xmm1(nyr_x64_mach_enc_t *e, int off, bool f32) {
  return mach_load_xmm(e, off, f32, 1);
}

static bool mach_sse_mov_reg(nyr_x64_mach_enc_t *e, unsigned dst, unsigned src,
                             bool f32) {
  if (dst >= 16 || src >= 16)
    return mach_err(e, "x64 machine form encode: unsupported XMM register");
  unsigned rex = 0x40 | (dst >= 8 ? 0x04 : 0) | (src >= 8 ? 0x01 : 0);
  return mach_u8(e, f32 ? 0xf3 : 0xf2) &&
         (rex != 0x40 ? mach_u8(e, rex) : true) && mach_u8(e, 0x0f) &&
         mach_u8(e, 0x10) &&
         mach_u8(e, (unsigned)(0xc0 | ((dst & 7) << 3) | (src & 7)));
}

static bool mach_sse_op_reg(nyr_x64_mach_enc_t *e, unsigned dst, unsigned src,
                            bool f32, unsigned opc) {
  if (dst >= 16 || src >= 16)
    return mach_err(e, "x64 machine form encode: unsupported XMM register");
  unsigned rex = 0x40 | (dst >= 8 ? 0x04 : 0) | (src >= 8 ? 0x01 : 0);
  return mach_u8(e, f32 ? 0xf3 : 0xf2) &&
         (rex != 0x40 ? mach_u8(e, rex) : true) && mach_u8(e, 0x0f) &&
         mach_u8(e, opc) &&
         mach_u8(e, (unsigned)(0xc0 | ((dst & 7) << 3) | (src & 7)));
}

static bool mach_sse_op_mem_reg(nyr_x64_mach_enc_t *e, unsigned dst, int off,
                                bool f32, unsigned opc) {
  if (dst >= 16)
    return mach_err(e, "x64 machine form encode: unsupported XMM register");
  return mach_u8(e, f32 ? 0xf3 : 0xf2) &&
         (dst >= 8 ? mach_rex(e, false, true, false, false) : true) &&
         mach_u8(e, 0x0f) && mach_u8(e, opc) &&
         mach_u8(e, (unsigned)(0x85 | ((dst & 7) << 3))) && mach_i32(e, off);
}

static bool mach_sse_cmp_reg(nyr_x64_mach_enc_t *e, unsigned lhs, unsigned rhs,
                             bool f32) {
  if (lhs >= 16 || rhs >= 16)
    return mach_err(e, "x64 machine form encode: unsupported XMM register");
  unsigned rex = 0x40 | (lhs >= 8 ? 0x04 : 0) | (rhs >= 8 ? 0x01 : 0);
  return (f32 ? true : mach_u8(e, 0x66)) &&
         (rex != 0x40 ? mach_u8(e, rex) : true) && mach_u8(e, 0x0f) &&
         mach_u8(e, 0x2e) &&
         mach_u8(e, (unsigned)(0xc0 | ((lhs & 7) << 3) | (rhs & 7)));
}

static bool mach_mov_rax_xmm(nyr_x64_mach_enc_t *e, unsigned dst) {
  if (dst >= 16)
    return mach_err(e, "x64 machine form encode: unsupported XMM register");
  return mach_u8(e, 0x66) && mach_rex(e, true, dst >= 8, false, false) &&
         mach_u8(e, 0x0f) && mach_u8(e, 0x6e) &&
         mach_u8(e, (unsigned)(0xc0 | ((dst & 7) << 3)));
}
static bool mach_mov_xmm_rax(nyr_x64_mach_enc_t *e, unsigned src) {
  if (src >= 16)
    return mach_err(e, "x64 machine form encode: unsupported XMM register");
  return mach_u8(e, 0x66) && mach_rex(e, true, src >= 8, false, false) &&
         mach_u8(e, 0x0f) && mach_u8(e, 0x7e) &&
         mach_u8(e, (unsigned)(0xc0 | ((src & 7) << 3)));
}

static bool mach_vextract_i128(nyr_x64_mach_enc_t *e, unsigned dst,
                               unsigned src, unsigned imm) {
  if (dst >= 16 || src >= 16)
    return mach_err(e, "x64 machine form encode: unsupported YMM register");
  /*
   * VEX.B maps the rm field (the xmm destination) and VEX.R the reg field
   * (the ymm source); a set bit means "no extension", so registers at or
   * above 8 must CLEAR their bit, not leave it set.
   */
  unsigned vex2 = 0xe3u & ~(src >= 8 ? 0x80u : 0u) & ~(dst >= 8 ? 0x20u : 0u);
  return mach_u8(e, 0xc4) && mach_u8(e, vex2) && mach_u8(e, 0x7d) &&
         mach_u8(e, 0x39) &&
         mach_u8(e, (unsigned)(0xc0 | ((src & 7) << 3) | (dst & 7))) &&
         mach_u8(e, imm & 1);
}

static bool mach_float_begin_segments(nyr_x64_mach_enc_t *e) {
  if (!e || !e->fp_fast_path || !e->fp_color_seeded)
    return true;
  for (uint32_t v = 0; v < e->fp_colors_len; ++v) {
    if (e->loop_fpr && e->loop_fpr[v] >= 0)
      continue;
    const nyr_mach_live_segment_t *seg = mach_fsegment_at(e, v, e->cur_inst);
    if (!seg || seg->start != e->cur_inst)
      continue;
    const nyr_mach_inst_t *in = &e->mach->insts[e->cur_inst];
    bool defines_value =
        in->dst.kind == NYR_MACH_OPERAND_VREG && in->dst.as.reg == v;
    if (e->loop_carry_seeding && seg->carried && !seg->spill &&
        seg->color >= 0) {
      e->fp_color_seeded[v] = !defines_value;
      continue;
    }
    e->fp_color_seeded[v] =
        mach_fsegment_carries(e, v, seg) && e->fp_color_seeded[v];
  }
  return true;
}

static bool mach_float_end_segments(nyr_x64_mach_enc_t *e) {
  if (!e || !e->fp_fast_path || !e->fp_color_seeded)
    return true;
  for (uint32_t v = 0; v < e->fp_colors_len; ++v) {
    if (e->loop_fpr && e->loop_fpr[v] >= 0)
      continue;
    const nyr_mach_live_segment_t *seg = mach_fsegment_at(e, v, e->cur_inst);
    if (!seg || seg->end != e->cur_inst)
      continue;
    const nyr_mach_live_segment_t *next =
        mach_fsegment_at(e, v, e->cur_inst + 1);
    bool carried = e->loop_carry_seeding && seg->carried && !seg->spill;
    bool needs_home =
        !carried &&
        (seg->spill || (next && !mach_fsegment_carries(e, v, next)));
    if (needs_home && seg->color >= 0 && e->fp_color_seeded[v]) {
      if (!mach_store_xmm(e, mach_vreg_home_off(e, v),
                          e->mach->vreg_types[v] == NYR_MACH_TYPE_F32,
                          (unsigned)seg->color))
        return false;
    }
    if (!carried && (seg->spill || !next || !mach_fsegment_carries(e, v, next)))
      e->fp_color_seeded[v] = false;
  }
  return true;
}

static const nyr_mach_live_segment_t *
mach_vsegment_at(const nyr_x64_mach_enc_t *e, uint32_t vreg, size_t inst) {
  return e ? nyr_mach_regalloc_segment_at(&e->vec_regalloc, vreg, inst) : NULL;
}

static bool mach_vsegment_carries(const nyr_x64_mach_enc_t *e, uint32_t vreg,
                                  const nyr_mach_live_segment_t *seg) {
  if (!e || !seg || seg->start == 0 || seg->reload || seg->color < 0)
    return false;
  const nyr_mach_live_segment_t *prev =
      mach_vsegment_at(e, vreg, seg->start - 1);
  /*
   * CFG joins use canonical stack homes. A register can keep the same color
   * across an edge without being resident in the destination block.
   */
  return prev && prev->block == seg->block && prev->end + 1 == seg->start &&
         !prev->spill && prev->color == seg->color;
}

static bool mach_vector_begin_segments(nyr_x64_mach_enc_t *e) {
  if (!e || !e->vec_fast_path || !e->vec_color_seeded)
    return true;
  for (uint32_t v = 0; v < e->vec_colors_len; ++v) {
    const nyr_mach_live_segment_t *seg = mach_vsegment_at(e, v, e->cur_inst);
    if (!seg || seg->start != e->cur_inst)
      continue;
    const nyr_mach_inst_t *in = &e->mach->insts[e->cur_inst];
    bool defines_value =
        in->dst.kind == NYR_MACH_OPERAND_VREG && in->dst.as.reg == v;
    if (e->loop_carry_seeding && seg->carried && !seg->spill &&
        seg->color >= 0) {
      e->vec_color_seeded[v] = !defines_value;
      continue;
    }
    e->vec_color_seeded[v] =
        mach_vsegment_carries(e, v, seg) && e->vec_color_seeded[v];
  }
  return true;
}

/*
 * A vector spill home is already authoritative when the SSA definition is in
 * another block and the current segment is marked spilled.  Such a value is
 * immutable: re-storing a register copy at the end of every loop iteration
 * only republishes the same canonical home.  Callers still use the regular
 * register path for same-block definitions, whose home may not be current
 * until the segment ends.
 */
static bool mach_vector_spill_home_is_current(const nyr_x64_mach_enc_t *e,
                                              uint32_t vreg) {
  if (!e || !e->mach || !e->def_inst || !e->cur_block_valid ||
      vreg >= e->vec_colors_len || e->cur_block >= e->mach->block_len)
    return false;
  const nyr_mach_live_segment_t *seg = mach_vsegment_at(e, vreg, e->cur_inst);
  if (!seg || !seg->spill)
    return false;
  size_t def = e->def_inst[vreg];
  if (def == SIZE_MAX || def >= e->mach->inst_len)
    return false;
  size_t def_block = mach_block_for_inst(e, def);
  return def_block != SIZE_MAX && def_block != e->cur_block;
}

static bool mach_vector_end_segments(nyr_x64_mach_enc_t *e) {
  if (!e || !e->vec_fast_path || !e->vec_color_seeded)
    return true;
  for (uint32_t v = 0; v < e->vec_colors_len; ++v) {
    const nyr_mach_live_segment_t *seg = mach_vsegment_at(e, v, e->cur_inst);
    if (!seg || seg->end != e->cur_inst)
      continue;
    const nyr_mach_live_segment_t *next =
        mach_vsegment_at(e, v, e->cur_inst + 1);
    bool carried = e->loop_carry_seeding && seg->carried && !seg->spill;
    bool needs_home =
        !carried &&
        (seg->spill || (next && !mach_vsegment_carries(e, v, next)));
    bool unchanged_spill_home =
        seg->spill && mach_vector_spill_home_is_current(e, v);
    if (needs_home && seg->color >= 0 && e->vec_color_seeded[v] &&
        !unchanged_spill_home) {
      bool is_v256 = mach_vreg_is_v256(e->mach, v);
      if (!(is_v256
                ? mach_ymm_store_v256(e, mach_vreg_home_off(e, v),
                                      (unsigned)(MVEC_PREG_BASE + seg->color))
                : mach_store_xmm_v128(e, mach_vreg_home_off(e, v),
                                      (unsigned)(MVEC_PREG_BASE + seg->color))))
        return false;
    }
    if (!carried && (seg->spill || !next || !mach_vsegment_carries(e, v, next)))
      e->vec_color_seeded[v] = false;
  }
  return true;
}

static bool mach_seed_vector_preg(nyr_x64_mach_enc_t *e, uint32_t vreg) {
  int preg = mach_vreg_vpreg(e, vreg);
  if (preg < 0 || (e->vec_color_seeded && e->vec_color_seeded[vreg]))
    return true;
  bool is_v256 = mach_vreg_is_v256(e->mach, vreg);
  if (!(is_v256
            ? mach_ymm_load_v256(e, mach_vreg_home_off(e, vreg), (unsigned)preg)
            : mach_load_xmm_v128(e, mach_vreg_home_off(e, vreg),
                                 (unsigned)preg)))
    return false;
  if (e->vec_color_seeded)
    e->vec_color_seeded[vreg] = true;
  return true;
}

static bool mach_load_vector_operand(nyr_x64_mach_enc_t *e,
                                     const nyr_mach_operand_t *op,
                                     unsigned scratch, unsigned *out_reg) {
  if (op && op->kind == NYR_MACH_OPERAND_VREG) {
    int preg = mach_vreg_vpreg(e, op->as.reg);
    if (preg >= 0) {
      if (!mach_seed_vector_preg(e, op->as.reg))
        return false;
      *out_reg = (unsigned)preg;
      return true;
    }
  }
  if (!op ||
      (op->kind != NYR_MACH_OPERAND_VREG && op->kind != NYR_MACH_OPERAND_FRAME))
    return mach_err(e, "x64 machine form encode: unsupported vector operand");
  *out_reg = scratch;
  /*
   * A spilled V256 vreg homes 32 bytes; loading only 128 bits would
   * truncate the upper half.  FRAME operands carry no type and are only
   * ever V128 in the vector fast path, so they keep the xmm load.
   */
  bool is_v256 = op->kind == NYR_MACH_OPERAND_VREG &&
                 mach_vreg_is_v256(e->mach, op->as.reg);
  if (is_v256)
    return mach_ymm_load_v256(e, mach_slot_off(e, op), scratch);
  return mach_load_xmm_v128(e, mach_slot_off(e, op), scratch);
}
static bool mach_load_float_operand(nyr_x64_mach_enc_t *e,
                                    const nyr_mach_operand_t *op, bool f32,
                                    unsigned scratch, unsigned *out_reg);

static bool mach_reduce_add_i64(nyr_x64_mach_enc_t *e,
                                const nyr_mach_inst_t *in, int dst_off) {
  if (!mach_load_op_rax(e, &in->src0) || !mach_mov_reg_rax(e, 2))
    return false;

  unsigned v = 14;
  if (!mach_load_vector_operand(e, &in->src1, 15, &v))
    return false;
  bool v256 = in->src1.kind == NYR_MACH_OPERAND_VREG && e->mach &&
              in->src1.as.reg < e->mach->vreg_len && e->mach->vreg_types &&
              e->mach->vreg_types[in->src1.as.reg] == NYR_MACH_TYPE_V256_I64;
  if (v256) {
    if (!mach_vextract_i128(e, 0, v, 0) || !mach_vextract_i128(e, 1, v, 1) ||
        !mach_vec_op_reg(e, 0, 1, 0x66, 0xd4))
      return false;
  } else if (v != 0 && !mach_vec_mov_reg(e, 0, v)) {
    return false;
  }
  if (!mach_u8(e, 0x66) || !mach_u8(e, 0x0f) || !mach_u8(e, 0x70) ||
      !mach_u8(e, 0xc8) || !mach_u8(e, 0x4e) ||
      !mach_vec_op_reg(e, 0, 1, 0x66, 0xd4) || !mach_mov_xmm_rax(e, 0) ||
      !mach_rex(e, true, false, false, false) || !mach_u8(e, 0x01) ||
      !mach_u8(e, 0xd0) || !mach_commit_vreg(e, &in->dst, dst_off))
    return false;
  return true;
}

static bool mach_reduce_add_f64(nyr_x64_mach_enc_t *e,
                                const nyr_mach_inst_t *in, int dst_off) {
  bool v256 = in->src1.kind == NYR_MACH_OPERAND_VREG &&
              mach_vreg_is_v256(e->mach, in->src1.as.reg) &&
              e->mach->vreg_types[in->src1.as.reg] == NYR_MACH_TYPE_V256_F64;
  if (v256) {
    /*
     * Four lanes on two reserved scratch registers: pair-wise add the
     * extracted halves, then fold the two partial sums into lane zero.
     */
    unsigned vec = 15;
    if (!mach_load_vector_operand(e, &in->src1, 14, &vec))
      return false;
    if (vec != 15 && !mach_ymm_mov_reg(e, 15, vec))
      return false;
    if (!mach_xmm_vextractf128(e, 14, 15, 0) ||
        !mach_xmm_vextractf128(e, 15, 15, 1))
      return false;
    /*
     * Sequential lane order start + l0 + l1 + l2 + l3 matches the NYIR
     * evaluator and the x86-64 NYIR encoder bit for bit; a pairwise tree
     * rounds differently. The first legacy-SSE write zeroes the upper half
     * of ymm15, so the {l2, l3} half is reloaded from the canonical home
     * before its lanes are consumed.
     */
    if (!mach_xmm_movhlps(e, 15, 14))
      return false;
    if (!mach_sse_op_reg(e, 14, 15, false, 0x58))
      return false;
    /*
     * A vector assigned to a YMM register may have no current stack home:
     * the vector fast path keeps resident values there until their live
     * segment ends. Reuse that still-live register after the scalar
     * instruction cleared ymm15's upper half; only spilled operands need a
     * reload from their canonical home.
     */
    if (vec == 14) {
      if (!mach_ymm_load_v256(e, mach_slot_off(e, &in->src1), 15))
        return false;
    } else if (!mach_ymm_mov_reg(e, 15, vec)) {
      return false;
    }
    if (!mach_xmm_vextractf128(e, 15, 15, 1))
      return false;
    if (!mach_sse_op_reg(e, 14, 15, false, 0x58))
      return false;
    if (!mach_xmm_movhlps(e, 15, 15))
      return false;
    if (!mach_sse_op_reg(e, 14, 15, false, 0x58))
      return false;
    unsigned scalar = 15;
    if (!mach_load_float_operand(e, &in->src0, false, 15, &scalar))
      return false;
    if (!mach_sse_op_reg(e, 14, scalar, false, 0x58))
      return false;
    int assigned = in->dst.kind == NYR_MACH_OPERAND_VREG
                       ? mach_vreg_fpreg(e, in->dst.as.reg)
                       : -1;
    if (assigned >= 0) {
      if ((unsigned)assigned != 14 &&
          !mach_sse_mov_reg(e, (unsigned)assigned, 14, false))
        return false;
      if (e->fp_color_seeded)
        e->fp_color_seeded[in->dst.as.reg] = true;
      return true;
    }
    return mach_store_xmm(e, dst_off, false, 14);
  }
  unsigned vec = 14;
  if (!mach_load_vector_operand(e, &in->src1, 14, &vec))
    return false;
  /*
   * Work in reserved scratch XMM14/XMM15 so a register-allocated vector
   * source remains live for later users.
   */
  if (vec != 14 && !mach_vec_mov_reg(e, 14, vec))
    return false;
  if (!mach_vec_mov_reg(e, 15, 14))
    return false;
  /*
   * shufpd $1, %xmm15, %xmm15: move the high f64 lane to lane zero.
   */
  unsigned rex = 0x40 | 0x04 | 0x01; /* R=1, B=1 for xmm15/xmm15 */
  if (!mach_u8(e, 0x66) || !mach_u8(e, rex) || !mach_u8(e, 0x0f) ||
      !mach_u8(e, 0xc6) || !mach_u8(e, 0xff) || !mach_u8(e, 0x01) ||
      !mach_sse_op_reg(e, 14, 15, false, 0x58))
    return false;

  unsigned scalar = 15;
  if (!mach_load_float_operand(e, &in->src0, false, 15, &scalar) ||
      !mach_sse_op_reg(e, 14, scalar, false, 0x58))
    return false;

  int assigned = in->dst.kind == NYR_MACH_OPERAND_VREG
                     ? mach_vreg_fpreg(e, in->dst.as.reg)
                     : -1;
  if (assigned >= 0) {
    if ((unsigned)assigned != 14 &&
        !mach_sse_mov_reg(e, (unsigned)assigned, 14, false))
      return false;
    if (e->fp_color_seeded)
      e->fp_color_seeded[in->dst.as.reg] = true;
    return true;
  }
  return mach_store_xmm(e, dst_off, false, 14);
}

static bool mach_vector_copy(nyr_x64_mach_enc_t *e,
                             const nyr_mach_operand_t *dst,
                             const nyr_mach_operand_t *src, int dst_off) {
  int assigned = dst && dst->kind == NYR_MACH_OPERAND_VREG
                     ? mach_vreg_vpreg(e, dst->as.reg)
                     : -1;
  unsigned d = assigned >= 0 ? (unsigned)assigned : 14;
  unsigned s = 14;
  if (!mach_load_vector_operand(e, src, d == 14 ? 15 : 14, &s))
    return false;
  if (s != d && !mach_vec_mov_reg(e, d, s))
    return false;
  if (assigned >= 0) {
    if (e->vec_color_seeded)
      e->vec_color_seeded[dst->as.reg] = true;
    return true;
  }
  return mach_store_xmm_v128(e, dst_off, d);
}

static bool mach_vector_load(nyr_x64_mach_enc_t *e, const nyr_mach_inst_t *in,
                             int dst_off) {
  int assigned = in->dst.kind == NYR_MACH_OPERAND_VREG
                     ? mach_vreg_vpreg(e, in->dst.as.reg)
                     : -1;
  unsigned d = assigned >= 0 ? (unsigned)assigned : 14;
  if (in->src0.kind == NYR_MACH_OPERAND_FRAME) {
    if (!mach_load_xmm_v128(e, mach_slot_off(e, &in->src0), d))
      return false;
  } else if (in->src0.kind == NYR_MACH_OPERAND_VREG) {
    if (!mach_load_op_rax(e, &in->src0) || !mach_load_xmm_v128_ptr(e, d))
      return false;
  } else {
    return mach_err(e, "x64 machine form encode: vector load from pointer");
  }
  if (assigned >= 0) {
    /* Keep the canonical home current for later split/spilled segments. */
    if (!mach_store_xmm_v128(e, dst_off, d))
      return false;
    if (e->vec_color_seeded)
      e->vec_color_seeded[in->dst.as.reg] = true;
    return true;
  }
  return mach_store_xmm_v128(e, dst_off, d);
}

static bool mach_vector_store(nyr_x64_mach_enc_t *e, const nyr_mach_inst_t *in,
                              int dst_off) {
  unsigned s = 14;
  if (!mach_load_vector_operand(e, &in->src0, 14, &s))
    return false;
  if (in->dst.kind == NYR_MACH_OPERAND_FRAME)
    return mach_store_xmm_v128(e, dst_off, s);
  if (in->dst.kind == NYR_MACH_OPERAND_VREG) {
    ny_x64_mem_addr_t addr;
    if (!mach_resolve_mem_addr(e, &in->dst, &addr) || !mach_u8(e, 0xf3) ||
        !mach_rex_for_addr(e, &addr, false, s >= 8) || !mach_u8(e, 0x0f) ||
        !mach_u8(e, 0x7f) || !mach_emit_mem(e, &addr, s & 7))
      return false;
    return true;
  }
  return mach_err(e, "x64 machine form encode: vector store destination");
}

static bool mach_vector_binop(nyr_x64_mach_enc_t *e, const nyr_mach_inst_t *in,
                              int dst_off, unsigned prefix, unsigned opc) {
  int assigned = in->dst.kind == NYR_MACH_OPERAND_VREG
                     ? mach_vreg_vpreg(e, in->dst.as.reg)
                     : -1;
  unsigned d = assigned >= 0 ? (unsigned)assigned : 14;
  unsigned a = 15;
  if (!mach_load_vector_operand(e, &in->src0, d == 15 ? 14 : 15, &a))
    return false;
  if (a != d && !mach_vec_mov_reg(e, d, a))
    return false;
  if (in->src1.kind == NYR_MACH_OPERAND_VREG) {
    int breg = mach_vreg_vpreg(e, in->src1.as.reg);
    if (breg >= 0) {
      if (!mach_seed_vector_preg(e, in->src1.as.reg) ||
          !mach_vec_op_reg(e, d, (unsigned)breg, prefix, opc))
        return false;
    } else if (!mach_vec_op_mem_reg(e, d, mach_slot_off(e, &in->src1), prefix,
                                    opc))
      return false;
  } else if (in->src1.kind == NYR_MACH_OPERAND_FRAME) {
    if (!mach_vec_op_mem_reg(e, d, mach_slot_off(e, &in->src1), prefix, opc))
      return false;
  } else
    return mach_err(e, "x64 machine form encode: unsupported vector rhs");
  if (assigned >= 0) {
    if (e->vec_color_seeded)
      e->vec_color_seeded[in->dst.as.reg] = true;
    return true;
  }
  return mach_store_xmm_v128(e, dst_off, d);
}

/*
 * V256_I64 ymm fast path wrappers
 */

/*
 * Copy for V256_I64: load src into a ymm reg, move to dst, spill if needed.
 */
static bool mach_vector_ymm_copy(nyr_x64_mach_enc_t *e,
                                 const nyr_mach_operand_t *dst,
                                 const nyr_mach_operand_t *src, int dst_off) {
  int assigned = dst && dst->kind == NYR_MACH_OPERAND_VREG
                     ? mach_vreg_vpreg(e, dst->as.reg)
                     : -1;
  unsigned d = assigned >= 0 ? (unsigned)assigned : 14;
  unsigned s = 14;
  /*
   * Use ymm load for V256 sources from frame/vreg homes.
   */
  if (src && src->kind == NYR_MACH_OPERAND_VREG) {
    int preg = mach_vreg_vpreg(e, src->as.reg);
    if (preg >= 0) {
      if (!mach_seed_vector_preg(e, src->as.reg))
        return false;
      s = (unsigned)preg;
    } else {
      s = d == 14 ? 15 : 14;
      if (!mach_ymm_load_v256(e, mach_slot_off(e, src), s))
        return false;
    }
  } else if (src && src->kind == NYR_MACH_OPERAND_FRAME) {
    s = d == 14 ? 15 : 14;
    if (!mach_ymm_load_v256(e, mach_slot_off(e, src), s))
      return false;
  } else {
    return mach_err(e, "x64: unsupported ymm copy source");
  }
  if (s != d && !mach_ymm_mov_reg(e, d, s))
    return false;
  if (assigned >= 0) {
    if (e->vec_color_seeded)
      e->vec_color_seeded[dst->as.reg] = true;
    return true;
  }
  return mach_ymm_store_v256(e, dst_off, d);
}

/*
 * Load for V256_I64 into a ymm register.
 *
 * vmovdqu ymmD, [base + index*scale] — VEX.256.F3.0F 6F with SIB addressing.
 * Lets a widened access consume its row-base and induction registers in
 * place instead of materializing the sum through %rax.
 */
static bool mach_ymm_load_v256_sib(nyr_x64_mach_enc_t *e, unsigned d,
                                   unsigned base, unsigned index,
                                   unsigned scale) {
  if (d >= 16 || base >= 16 || index >= 16)
    return mach_err(e, "x64: unsupported YMM SIB operand");
  unsigned char vex2 = 0xe1; /* R=X=B=1 (unused), mmmmm=0001 (0F) */
  if (d >= 8)
    vex2 = (unsigned char)(vex2 & ~0x80u);
  if (index >= 8)
    vex2 = (unsigned char)(vex2 & ~0x40u); /* inverted X: SIB index */
  if (base >= 8)
    vex2 = (unsigned char)(vex2 & ~0x20u); /* inverted B: SIB base */
  unsigned log2 = scale == 2 ? 1u : scale == 4 ? 2u : scale == 8 ? 3u : 0u;
  unsigned mod = ((base & 7) == 4 || (base & 7) == 5) ? 1u : 0u;
  return mach_u8(e, 0xc4) && mach_u8(e, vex2) && mach_u8(e, 0x7e) &&
         mach_u8(e, 0x6f) &&
         mach_u8(e, (unsigned char)((mod << 6) | ((d & 7) << 3) | 0x04)) &&
         mach_u8(e, (unsigned char)((log2 << 6) | ((index & 7) << 3) |
                                    (base & 7))) &&
         (mod ? mach_u8(e, 0x00) : true);
}

/*
 * vmovdqu [base + index*scale], ymmS — VEX.256.F3.0F 7F with SIB addressing.
 */
static bool mach_ymm_store_v256_sib(nyr_x64_mach_enc_t *e, unsigned s,
                                    unsigned base, unsigned index,
                                    unsigned scale) {
  if (s >= 16 || base >= 16 || index >= 16)
    return mach_err(e, "x64: unsupported YMM SIB operand");
  unsigned char vex2 = 0xe1;
  if (s >= 8)
    vex2 = (unsigned char)(vex2 & ~0x80u);
  if (index >= 8)
    vex2 = (unsigned char)(vex2 & ~0x40u);
  if (base >= 8)
    vex2 = (unsigned char)(vex2 & ~0x20u);
  unsigned log2 = scale == 2 ? 1u : scale == 4 ? 2u : scale == 8 ? 3u : 0u;
  unsigned mod = ((base & 7) == 4 || (base & 7) == 5) ? 1u : 0u;
  return mach_u8(e, 0xc4) && mach_u8(e, vex2) && mach_u8(e, 0x7e) &&
         mach_u8(e, 0x7f) &&
         mach_u8(e, (unsigned char)((mod << 6) | ((s & 7) << 3) | 0x04)) &&
         mach_u8(e, (unsigned char)((log2 << 6) | ((index & 7) << 3) |
                                    (base & 7))) &&
         (mod ? mach_u8(e, 0x00) : true);
}

static bool mach_vector_ymm_load(nyr_x64_mach_enc_t *e,
                                 const nyr_mach_inst_t *in, int dst_off) {
  int assigned = in->dst.kind == NYR_MACH_OPERAND_VREG
                     ? mach_vreg_vpreg(e, in->dst.as.reg)
                     : -1;
  unsigned d = assigned >= 0 ? (unsigned)assigned : 14;
  if (in->src0.kind == NYR_MACH_OPERAND_FRAME) {
    if (!mach_ymm_load_v256(e, mach_slot_off(e, &in->src0), d))
      return false;
  } else if (in->src0.kind == NYR_MACH_OPERAND_VREG) {
    unsigned b = 0, i = 0, s = 0;
    if (mach_sib_query(e, &in->src0, &b, &i, &s)) {
      if (!mach_ymm_load_v256_sib(e, d, b, i, s))
        return false;
    } else if (!mach_load_op_rax(e, &in->src0) ||
               !mach_ymm_load_v256_ptr(e, d)) {
      return false;
    }
  } else {
    return mach_err(e, "x64: ymm load from pointer");
  }
  if (assigned >= 0) {
    if (e->vec_color_seeded)
      e->vec_color_seeded[in->dst.as.reg] = true;
    return true;
  }
  return mach_ymm_store_v256(e, dst_off, d);
}

/*
 * Store for V256_I64: write ymm reg to memory.
 */
static bool mach_vector_ymm_store(nyr_x64_mach_enc_t *e,
                                  const nyr_mach_inst_t *in, int dst_off) {
  unsigned s = 14;
  if (in->src0.kind == NYR_MACH_OPERAND_VREG) {
    int preg = mach_vreg_vpreg(e, in->src0.as.reg);
    if (preg >= 0) {
      if (!mach_seed_vector_preg(e, in->src0.as.reg))
        return false;
      s = (unsigned)preg;
    } else if (!mach_ymm_load_v256(e, mach_slot_off(e, &in->src0), s))
      return false;
  } else if (in->src0.kind == NYR_MACH_OPERAND_FRAME) {
    if (!mach_ymm_load_v256(e, mach_slot_off(e, &in->src0), s))
      return false;
  } else {
    return mach_err(e, "x64: ymm store source");
  }
  if (in->dst.kind == NYR_MACH_OPERAND_FRAME)
    return mach_ymm_store_v256(e, dst_off, s);
  if (in->dst.kind == NYR_MACH_OPERAND_VREG) {
    unsigned b = 0, i = 0, sc = 0;
    if (mach_sib_query(e, &in->dst, &b, &i, &sc)) {
      if (!mach_ymm_store_v256_sib(e, s, b, i, sc))
        return false;
      return true;
    }
    if (!mach_load_op_rax(e, &in->dst) || !mach_ymm_store_v256_ptr(e, s))
      return false;
    return true;
  }
  return mach_err(e, "x64: ymm store destination");
}

/*
 * Binary op for V256_I64: vpaddq/vpsubq/vpand/vpor/vpxor ymm
 */
static bool mach_vector_ymm_binop(nyr_x64_mach_enc_t *e,
                                  const nyr_mach_inst_t *in, int dst_off,
                                  unsigned opc) {
  int assigned = in->dst.kind == NYR_MACH_OPERAND_VREG
                     ? mach_vreg_vpreg(e, in->dst.as.reg)
                     : -1;
  unsigned d = assigned >= 0 ? (unsigned)assigned : 14;
  unsigned a = 15;
  /*
   * Load src0 into a ymm reg.
   */
  if (in->src0.kind == NYR_MACH_OPERAND_VREG) {
    int preg = mach_vreg_vpreg(e, in->src0.as.reg);
    if (preg >= 0) {
      if (!mach_seed_vector_preg(e, in->src0.as.reg))
        return false;
      a = (unsigned)preg;
    } else {
      a = d == 15 ? 14 : 15;
      if (!mach_ymm_load_v256(e, mach_slot_off(e, &in->src0), a))
        return false;
    }
  } else if (in->src0.kind == NYR_MACH_OPERAND_FRAME) {
    a = d == 15 ? 14 : 15;
    if (!mach_ymm_load_v256(e, mach_slot_off(e, &in->src0), a))
      return false;
  } else {
    return mach_err(e, "x64: unsupported ymm binop src0");
  }
  if (a != d && !mach_ymm_mov_reg(e, d, a))
    return false;
  /*
   * Apply src1.
   */
  if (in->src1.kind == NYR_MACH_OPERAND_VREG) {
    int breg = mach_vreg_vpreg(e, in->src1.as.reg);
    if (breg >= 0) {
      if (!mach_seed_vector_preg(e, in->src1.as.reg) ||
          !mach_ymm_op_reg(e, d, (unsigned)breg, opc))
        return false;
    } else if (!mach_ymm_op_mem_reg(e, d, mach_slot_off(e, &in->src1), opc))
      return false;
  } else if (in->src1.kind == NYR_MACH_OPERAND_FRAME) {
    if (!mach_ymm_op_mem_reg(e, d, mach_slot_off(e, &in->src1), opc))
      return false;
  } else {
    return mach_err(e, "x64: unsupported ymm binop src1");
  }
  if (assigned >= 0) {
    if (e->vec_color_seeded)
      e->vec_color_seeded[in->dst.as.reg] = true;
    return true;
  }
  return mach_ymm_store_v256(e, dst_off, d);
}

static bool mach_vector_shift(nyr_x64_mach_enc_t *e, const nyr_mach_inst_t *in,
                              int dst_off, unsigned prefix, unsigned opc) {
  int assigned = in->dst.kind == NYR_MACH_OPERAND_VREG
                     ? mach_vreg_vpreg(e, in->dst.as.reg)
                     : -1;
  unsigned d = assigned >= 0 ? (unsigned)assigned : 14;
  unsigned a = d == 14 ? 15 : 14;
  if (!mach_load_vector_operand(e, &in->src0, a, &a))
    return false;
  if (a != d && !mach_vec_mov_reg(e, d, a))
    return false;
  if (in->src1.kind == NYR_MACH_OPERAND_VREG) {
    int breg = mach_vreg_vpreg(e, in->src1.as.reg);
    if (breg >= 0) {
      if (!mach_seed_vector_preg(e, in->src1.as.reg) ||
          !mach_vec_op_reg(e, d, (unsigned)breg, prefix, opc))
        return false;
    } else if (!mach_vec_op_mem_reg(e, d, mach_slot_off(e, &in->src1), prefix,
                                    opc))
      return false;
  } else if (in->src1.kind == NYR_MACH_OPERAND_FRAME) {
    if (!mach_vec_op_mem_reg(e, d, mach_slot_off(e, &in->src1), prefix, opc))
      return false;
  } else
    return mach_err(e, "x64 machine form encode: unsupported vector shift rhs");
  if (assigned >= 0) {
    if (e->vec_color_seeded)
      e->vec_color_seeded[in->dst.as.reg] = true;
    return true;
  }
  return mach_store_xmm_v128(e, dst_off, d);
}

static bool mach_vector_fma(nyr_x64_mach_enc_t *e, const nyr_mach_inst_t *in,
                            int dst_off, unsigned prefix, unsigned mul_op,
                            unsigned add_op) {
  if (!mach_vector_binop(e, in, dst_off, prefix, mul_op))
    return false;
  int assigned = in->dst.kind == NYR_MACH_OPERAND_VREG
                     ? mach_vreg_vpreg(e, in->dst.as.reg)
                     : -1;
  unsigned d = assigned >= 0 ? (unsigned)assigned : 14;
  const nyr_mach_operand_t *src2 = &in->src2;
  if (src2->kind == NYR_MACH_OPERAND_VREG) {
    int creg = mach_vreg_vpreg(e, src2->as.reg);
    if (creg >= 0) {
      if (!mach_seed_vector_preg(e, src2->as.reg) ||
          !mach_vec_op_reg(e, d, (unsigned)creg, prefix, add_op))
        return false;
    } else if (!mach_vec_op_mem_reg(e, d, mach_slot_off(e, src2), prefix,
                                    add_op))
      return false;
  } else if (src2->kind == NYR_MACH_OPERAND_FRAME) {
    if (!mach_vec_op_mem_reg(e, d, mach_slot_off(e, src2), prefix, add_op))
      return false;
  } else
    return mach_err(e, "x64 machine form encode: unsupported vector FMA rhs");
  if (assigned >= 0 && e->vec_color_seeded)
    e->vec_color_seeded[in->dst.as.reg] = true;
  return assigned >= 0 || mach_store_xmm_v128(e, dst_off, d);
}

/*
 * V256_F64 fast-path binary op: VEX.256.66.0F float forms (vaddpd family)
 * with the accumulator carried explicitly in VEX.vvvv.
 */
static bool mach_vector_ymm_rhs_load(const nyr_x64_mach_enc_t *e,
                                     size_t load_index, size_t consumer_index,
                                     const nyr_mach_operand_t *rhs) {
  if (!e || !e->mach || !rhs || load_index >= e->mach->inst_len ||
      consumer_index >= e->mach->inst_len || consumer_index != load_index + 1 ||
      mach_block_for_inst(e, load_index) !=
          mach_block_for_inst(e, consumer_index))
    return false;
  const nyr_mach_inst_t *load = &e->mach->insts[load_index];
  const nyr_mach_inst_t *consumer = &e->mach->insts[consumer_index];
  if (load->opcode != NYR_MACH_LOAD ||
      load->dst.kind != NYR_MACH_OPERAND_VREG ||
      mach_v128_type(e->mach, &load->dst) != NYR_MACH_TYPE_V256_F64 ||
      (consumer->opcode != NYR_MACH_ADD && consumer->opcode != NYR_MACH_SUB &&
       consumer->opcode != NYR_MACH_MUL && consumer->opcode != NYR_MACH_DIV) ||
      mach_v128_type(e->mach, &consumer->dst) != NYR_MACH_TYPE_V256_F64 ||
      !mach_op_is_vreg(rhs, load->dst.as.reg) ||
      mach_inst_input_uses_vreg(consumer, load->dst.as.reg) != 1 ||
      mach_op_is_vreg(&consumer->src0, load->dst.as.reg) ||
      mach_op_is_vreg(&consumer->src2, load->dst.as.reg))
    return false;
  for (size_t i = consumer_index + 1; i < e->mach->inst_len; ++i)
    if (mach_inst_uses_vreg(&e->mach->insts[i], load->dst.as.reg))
      return false;

  /* Initially restrict the fold to addresses the current YMM load path can
   * encode without materializing a scratch address. */
  if (load->src0.kind == NYR_MACH_OPERAND_FRAME)
    return true;
  if (load->src0.kind != NYR_MACH_OPERAND_VREG)
    return false;
  const nyr_mach_operand_t *base = NULL, *index = NULL;
  unsigned scale = 0;
  if (!mach_sib_lea_mem(e, load->src0.as.reg, load_index, &base, &index, &scale,
                        NULL) ||
      base->kind != NYR_MACH_OPERAND_VREG ||
      index->kind != NYR_MACH_OPERAND_VREG ||
      mach_vreg_preg(e, base->as.reg) < 0 ||
      mach_vreg_preg(e, index->as.reg) < 0 ||
      mach_vreg_preg(e, index->as.reg) == 4 ||
      (scale != 1 && scale != 2 && scale != 4 && scale != 8))
    return false;
  return true;
}

static bool mach_vector_ymm_fp_binop(nyr_x64_mach_enc_t *e,
                                     const nyr_mach_inst_t *in, int dst_off,
                                     unsigned opc) {
  int assigned = in->dst.kind == NYR_MACH_OPERAND_VREG
                     ? mach_vreg_vpreg(e, in->dst.as.reg)
                     : -1;
  unsigned d = assigned >= 0 ? (unsigned)assigned : 14;
  unsigned a_reg = d;
  if (in->src0.kind == NYR_MACH_OPERAND_VREG) {
    int preg = mach_vreg_vpreg(e, in->src0.as.reg);
    if (preg >= 0) {
      if (!mach_seed_vector_preg(e, in->src0.as.reg))
        return false;
      a_reg = (unsigned)preg;
    } else if (!mach_ymm_load_v256(e, mach_slot_off(e, &in->src0), d))
      return false;
  } else if (in->src0.kind == NYR_MACH_OPERAND_FRAME) {
    if (!mach_ymm_load_v256(e, mach_slot_off(e, &in->src0), d))
      return false;
  } else {
    return mach_err(e, "x64: unsupported ymm f64 binop src0");
  }
  /* AVX is three-operand here: VEX.vvvv carries src0, so don't copy it to dst.
   */
  unsigned char vex3 = (unsigned char)((((~a_reg) & 0xfu) << 3) | 0x05u);
  const nyr_mach_inst_t *folded_load = NULL;
  if (e->cur_inst > 0 &&
      mach_vector_ymm_rhs_load(e, e->cur_inst - 1, e->cur_inst, &in->src1))
    folded_load = &e->mach->insts[e->cur_inst - 1];
  if (folded_load && folded_load->src0.kind == NYR_MACH_OPERAND_FRAME) {
    unsigned char vex2 = (unsigned char)(d >= 8 ? 0x61 : 0xe1);
    if (!mach_u8(e, 0xc4) || !mach_u8(e, vex2) || !mach_u8(e, vex3) ||
        !mach_u8(e, opc) || !mach_u8(e, 0x85u | ((d & 7) << 3)) ||
        !mach_i32(e, mach_slot_off(e, &folded_load->src0)))
      return false;
  } else if (folded_load) {
    const nyr_mach_operand_t *base_op = NULL, *index_op = NULL;
    unsigned scale = 0;
    unsigned base = 0, index = 0;
    size_t saved_inst = e->cur_inst;
    e->cur_inst--;
    bool addr_ok = mach_sib_lea_mem(e, folded_load->src0.as.reg, e->cur_inst,
                                    &base_op, &index_op, &scale, NULL);
    if (addr_ok) {
      int bp = mach_vreg_preg(e, base_op->as.reg);
      int ip = mach_vreg_preg(e, index_op->as.reg);
      addr_ok = bp >= 0 && ip >= 0 && ip != 4;
      if (addr_ok) {
        base = (unsigned)bp;
        index = (unsigned)ip;
      }
    }
    e->cur_inst = saved_inst;
    if (!addr_ok)
      return mach_err(e, "x64: lost resident ymm rhs address");
    unsigned char vex2 = (unsigned char)(d >= 8 ? 0x61 : 0xe1);
    if (base >= 8)
      vex2 = (unsigned char)(vex2 & ~0x20u);
    if (index >= 8)
      vex2 = (unsigned char)(vex2 & ~0x40u);
    ny_x64_mem_addr_t mem = {base, (int)index, scale, 0};
    if (!mach_u8(e, 0xc4) || !mach_u8(e, vex2) || !mach_u8(e, vex3) ||
        !mach_u8(e, opc) || !mach_emit_mem(e, &mem, d))
      return false;
  } else if (in->src1.kind == NYR_MACH_OPERAND_VREG) {
    int breg = mach_vreg_vpreg(e, in->src1.as.reg);
    if (breg >= 0) {
      if (!mach_seed_vector_preg(e, in->src1.as.reg))
        return false;
      unsigned char vex2 = 0xe1;
      if (d >= 8)
        vex2 = (unsigned char)(vex2 & ~0x80u);
      if (breg >= 8)
        vex2 = (unsigned char)(vex2 & ~0x20u);
      if (!mach_u8(e, 0xc4) || !mach_u8(e, vex2) || !mach_u8(e, vex3) ||
          !mach_u8(e, opc) ||
          !mach_u8(e, 0xc0u | ((d & 7) << 3) | ((unsigned)breg & 7)))
        return false;
    } else if (!mach_u8(e, 0xc4) || !mach_u8(e, d >= 8 ? 0x61 : 0xe1) ||
               !mach_u8(e, vex3) || !mach_u8(e, opc) ||
               !mach_u8(e, 0x85u | ((d & 7) << 3)) ||
               !mach_i32(e, mach_slot_off(e, &in->src1)))
      return false;
  } else if (in->src1.kind == NYR_MACH_OPERAND_FRAME) {
    if (!mach_u8(e, 0xc4) || !mach_u8(e, d >= 8 ? 0x61 : 0xe1) ||
        !mach_u8(e, vex3) || !mach_u8(e, opc) ||
        !mach_u8(e, 0x85u | ((d & 7) << 3)) ||
        !mach_i32(e, mach_slot_off(e, &in->src1)))
      return false;
  } else {
    return mach_err(e, "x64: unsupported ymm f64 binop src1");
  }
  if (assigned >= 0) {
    if (e->vec_color_seeded)
      e->vec_color_seeded[in->dst.as.reg] = true;
    return true;
  }
  return mach_ymm_store_v256(e, dst_off, d);
}

/*
 * V256_F64 fused multiply-add: d = src1 * d + src2 in one rounding via
 * vfmadd213pd. The lowerer only admits this opcode on fused-FMA targets,
 * so the encoder may assume the host decodes FMA3.
 */
static bool mach_vector_ymm_fma(nyr_x64_mach_enc_t *e,
                                const nyr_mach_inst_t *in, int dst_off) {
  int assigned = in->dst.kind == NYR_MACH_OPERAND_VREG
                     ? mach_vreg_vpreg(e, in->dst.as.reg)
                     : -1;
  unsigned d = assigned >= 0 ? (unsigned)assigned : 14;
  if (in->src0.kind == NYR_MACH_OPERAND_VREG) {
    int preg = mach_vreg_vpreg(e, in->src0.as.reg);
    if (preg >= 0) {
      if (!mach_seed_vector_preg(e, in->src0.as.reg))
        return false;
      if ((unsigned)preg != d && !mach_ymm_mov_reg(e, d, (unsigned)preg))
        return false;
    } else if (!mach_ymm_load_v256(e, mach_slot_off(e, &in->src0), d))
      return false;
  } else if (in->src0.kind == NYR_MACH_OPERAND_FRAME) {
    if (!mach_ymm_load_v256(e, mach_slot_off(e, &in->src0), d))
      return false;
  } else {
    return mach_err(e, "x64: unsupported ymm f64 fma src0");
  }
  /*
   * The 213 form takes the multiply source from VEX.vvvv, which must be a
   * register; stage b in the spare reserved register when it has none.
   */
  unsigned mul = d == 15 ? 14 : 15;
  if (in->src1.kind == NYR_MACH_OPERAND_VREG) {
    int breg = mach_vreg_vpreg(e, in->src1.as.reg);
    if (breg >= 0) {
      if (!mach_seed_vector_preg(e, in->src1.as.reg))
        return false;
      mul = (unsigned)breg;
    } else if (!mach_ymm_load_v256(e, mach_slot_off(e, &in->src1), mul))
      return false;
  } else if (in->src1.kind == NYR_MACH_OPERAND_FRAME) {
    if (!mach_ymm_load_v256(e, mach_slot_off(e, &in->src1), mul))
      return false;
  } else {
    return mach_err(e, "x64: unsupported ymm f64 fma src1");
  }
  if (in->src2.kind == NYR_MACH_OPERAND_VREG) {
    int creg = mach_vreg_vpreg(e, in->src2.as.reg);
    if (creg >= 0) {
      if (!mach_seed_vector_preg(e, in->src2.as.reg) ||
          !mach_ymm_vfmadd213_reg(e, d, mul, (unsigned)creg))
        return false;
    } else if (!mach_ymm_vfmadd213_mem(e, d, mul, mach_slot_off(e, &in->src2)))
      return false;
  } else if (in->src2.kind == NYR_MACH_OPERAND_FRAME) {
    if (!mach_ymm_vfmadd213_mem(e, d, mul, mach_slot_off(e, &in->src2)))
      return false;
  } else {
    return mach_err(e, "x64: unsupported ymm f64 fma src2");
  }
  if (assigned >= 0) {
    if (e->vec_color_seeded)
      e->vec_color_seeded[in->dst.as.reg] = true;
    return true;
  }
  return mach_ymm_store_v256(e, dst_off, d);
}

static bool mach_vector_fast_eligible(const nyr_mach_func_t *mach);

/*
 * True when the function carries any vector-class vreg. Used to decide
 * whether scalar float colors must stay below the vector XMM half and
 * whether slow-path vector lowering (xmm0-xmm3 scratch) forbids scalar
 * float residency.
 */
static bool mach_vector_types_present(const nyr_mach_func_t *mach) {
  if (!mach)
    return false;
  for (size_t i = 0; i < mach->vreg_len; ++i) {
    nyr_mach_type_t t = mach->vreg_types[i];
    if (t == NYR_MACH_TYPE_V128_I64 || t == NYR_MACH_TYPE_V128_F64 ||
        t == NYR_MACH_TYPE_V128_F32 || t == NYR_MACH_TYPE_V256_I64 ||
        t == NYR_MACH_TYPE_V256_F64)
      return true;
  }
  return false;
}

static bool mach_vector_fast_eligible(const nyr_mach_func_t *mach) {
  if (!mach)
    return false;
  if (!mach_vector_types_present(mach))
    return false;
  for (size_t i = 0; i < mach->inst_len; ++i) {
    const nyr_mach_inst_t *in = &mach->insts[i];
    if (in->opcode == NYR_MACH_CALL) {
      /*
       * Scalar calls may clobber every XMM register. Keep vector homes
       * canonical around them, but leave vector argument/return ABI shapes
       * to the dedicated ABI lowering path.
       */
      if (mach_is_v128(mach, &in->dst))
        return false;
      for (size_t ai = 0; ai < in->args_len; ++ai)
        if (mach_is_v128(mach, &in->args[ai]))
          return false;
      continue;
    }
    const nyr_mach_operand_t *ops[] = {&in->dst, &in->src0, &in->src1,
                                       &in->src2};
    bool vector_inst = false;
    for (size_t k = 0; k < sizeof(ops) / sizeof(ops[0]); ++k)
      if (mach_is_v128(mach, ops[k]))
        vector_inst = true;
    if (!vector_inst)
      continue;
    if (in->opcode == NYR_MACH_RET)
      return false;
    /*
     * A general four-lane f64 permutation needs more xmm scratch than the
     * two reserved vector registers; keep the shuffle on the home path.
     */
    if (in->opcode == NYR_MACH_SHUFFLE &&
        mach_v128_type(mach, &in->dst) == NYR_MACH_TYPE_V256_F64)
      return false;
    if (in->opcode == NYR_MACH_DIV &&
        mach_v128_type(mach, &in->dst) == NYR_MACH_TYPE_V128_I64)
      return false;
    if (in->opcode == NYR_MACH_COPY && in->src0.kind != NYR_MACH_OPERAND_VREG &&
        in->src0.kind != NYR_MACH_OPERAND_FRAME)
      return false;
  }
  return true;
}

static bool mach_save_float_live_out(nyr_x64_mach_enc_t *e, size_t block) {
  if (!e || !e->fp_fast_path || block >= e->mach->block_len)
    return true;
  const nyr_mach_block_t *b = &e->mach->blocks[block];
  if (!b->inst_count)
    return true;
  size_t last = b->first_inst + b->inst_count - 1;
  e->cur_inst = last;
  for (uint32_t v = 0; v < e->fp_colors_len; ++v) {
    if (!nyr_mach_regalloc_live_out(&e->fp_regalloc, block, v) ||
        !e->fp_color_seeded || !e->fp_color_seeded[v])
      continue;
    if (e->loop_fpr && e->loop_fpr[v] >= 0)
      continue;
    const nyr_mach_live_segment_t *seg = mach_fsegment_at(e, v, last);
    if (!seg || seg->color < 0)
      continue;
    if (e->loop_carry_seeding && seg->carried && !seg->spill)
      continue;
    if (!mach_store_xmm(e, mach_vreg_home_off(e, v),
                        e->mach->vreg_types[v] == NYR_MACH_TYPE_F32,
                        (unsigned)seg->color))
      return false;
    e->fp_color_seeded[v] = false;
  }
  return true;
}

static bool mach_save_vector_live_out(nyr_x64_mach_enc_t *e, size_t block) {
  if (!e || !e->vec_fast_path || block >= e->mach->block_len)
    return true;
  const nyr_mach_block_t *b = &e->mach->blocks[block];
  if (!b->inst_count)
    return true;
  size_t last = b->first_inst + b->inst_count - 1;
  e->cur_inst = last;
  for (uint32_t v = 0; v < e->vec_colors_len; ++v) {
    if (!nyr_mach_regalloc_live_out(&e->vec_regalloc, block, v) ||
        !e->vec_color_seeded || !e->vec_color_seeded[v])
      continue;
    const nyr_mach_live_segment_t *seg = mach_vsegment_at(e, v, last);
    if (!seg || seg->color < 0)
      continue;
    if (e->loop_carry_seeding && seg->carried && !seg->spill)
      continue;
    bool is_v256 = mach_vreg_is_v256(e->mach, v);
    if (!(is_v256
              ? mach_ymm_store_v256(e, mach_vreg_home_off(e, v),
                                    (unsigned)(MVEC_PREG_BASE + seg->color))
              : mach_store_xmm_v128(e, mach_vreg_home_off(e, v),
                                    (unsigned)(MVEC_PREG_BASE + seg->color))))
      return false;
    e->vec_color_seeded[v] = false;
  }
  return true;
}

static bool mach_flush_float_to_home(nyr_x64_mach_enc_t *e) {
  if (!e || !e->fp_fast_path || !e->fp_color_seeded)
    return true;
  for (uint32_t v = 0; v < e->fp_colors_len; ++v) {
    if (!e->fp_color_seeded[v])
      continue;
    if (e->loop_fpr && e->loop_fpr[v] >= 0) {
      e->fp_color_seeded[v] = false;
      continue;
    }
    const nyr_mach_live_segment_t *seg = mach_fsegment_at(e, v, e->cur_inst);
    if (!seg || seg->color < 0)
      continue;
    if (!mach_store_xmm(e, mach_vreg_home_off(e, v),
                        e->mach->vreg_types[v] == NYR_MACH_TYPE_F32,
                        (unsigned)seg->color))
      return false;
    e->fp_color_seeded[v] = false;
  }
  return true;
}

static bool mach_flush_vector_to_home(nyr_x64_mach_enc_t *e) {
  if (!e || !e->vec_fast_path || !e->vec_color_seeded)
    return true;
  for (uint32_t v = 0; v < e->vec_colors_len; ++v) {
    if (!e->vec_color_seeded[v])
      continue;
    const nyr_mach_live_segment_t *seg = mach_vsegment_at(e, v, e->cur_inst);
    if (!seg || seg->color < 0)
      continue;
    bool is_v256 = mach_vreg_is_v256(e->mach, v);
    if (!(is_v256
              ? mach_ymm_store_v256(e, mach_vreg_home_off(e, v),
                                    (unsigned)(MVEC_PREG_BASE + seg->color))
              : mach_store_xmm_v128(e, mach_vreg_home_off(e, v),
                                    (unsigned)(MVEC_PREG_BASE + seg->color))))
      return false;
    e->vec_color_seeded[v] = false;
  }
  return true;
}

static bool mach_seed_float_preg(nyr_x64_mach_enc_t *e, uint32_t vreg) {
  int preg = mach_vreg_fpreg(e, vreg);
  if (preg < 0 || (e->fp_color_seeded && e->fp_color_seeded[vreg]))
    return true;
  if (!mach_load_xmm(e, mach_vreg_home_off(e, vreg),
                     e->mach->vreg_types[vreg] == NYR_MACH_TYPE_F32,
                     (unsigned)preg))
    return false;
  if (e->fp_color_seeded)
    e->fp_color_seeded[vreg] = true;
  return true;
}

static bool mach_load_float_operand(nyr_x64_mach_enc_t *e,
                                    const nyr_mach_operand_t *op, bool f32,
                                    unsigned scratch, unsigned *out_reg) {
  if (op && op->kind == NYR_MACH_OPERAND_VREG) {
    int preg = mach_vreg_fpreg(e, op->as.reg);
    if (preg >= 0) {
      if (!mach_seed_float_preg(e, op->as.reg))
        return false;
      *out_reg = (unsigned)preg;
      return true;
    }
  }
  if (!op ||
      (op->kind != NYR_MACH_OPERAND_VREG && op->kind != NYR_MACH_OPERAND_FRAME))
    return mach_err(e, "x64 machine form encode: unsupported float operand");
  *out_reg = scratch;
  return mach_load_xmm(e, mach_slot_off(e, op), f32, scratch);
}

static bool mach_float_copy(nyr_x64_mach_enc_t *e,
                            const nyr_mach_operand_t *dst,
                            const nyr_mach_operand_t *src, int dst_off,
                            bool f32) {
  unsigned d = 14, s = 14;
  int assigned = dst && dst->kind == NYR_MACH_OPERAND_VREG
                     ? mach_vreg_fpreg(e, dst->as.reg)
                     : -1;
  if (assigned >= 0)
    d = (unsigned)assigned;
  if (src && src->kind == NYR_MACH_OPERAND_IMM) {
    if (!mach_mov_rax_imm(e, src->as.imm) || !mach_mov_rax_xmm(e, d))
      return false;
  } else if (!mach_load_float_operand(e, src, f32, 15, &s))
    return false;
  else if (s != d && !mach_sse_mov_reg(e, d, s, f32))
    return false;
  if (assigned >= 0) {
    if (e->fp_color_seeded)
      e->fp_color_seeded[dst->as.reg] = true;
    return true;
  }
  return mach_store_xmm(e, dst_off, f32, d);
}

static bool mach_float_binop(nyr_x64_mach_enc_t *e, const nyr_mach_inst_t *in,
                             int dst_off, bool f32, unsigned opc) {
  unsigned d = 14, a = 14;
  int assigned = in->dst.kind == NYR_MACH_OPERAND_VREG
                     ? mach_vreg_fpreg(e, in->dst.as.reg)
                     : -1;
  if (assigned >= 0)
    d = (unsigned)assigned;
  if (!mach_load_float_operand(e, &in->src0, f32, d, &a))
    return false;
  if (a != d && !mach_sse_mov_reg(e, d, a, f32))
    return false;
  if (in->src1.kind == NYR_MACH_OPERAND_VREG) {
    int breg = mach_vreg_fpreg(e, in->src1.as.reg);
    if (breg >= 0) {
      if (!mach_seed_float_preg(e, in->src1.as.reg) ||
          !mach_sse_op_reg(e, d, (unsigned)breg, f32, opc))
        return false;
    } else if (!mach_sse_op_mem_reg(e, d, mach_slot_off(e, &in->src1), f32,
                                    opc))
      return false;
  } else if (in->src1.kind == NYR_MACH_OPERAND_FRAME) {
    if (!mach_sse_op_mem_reg(e, d, mach_slot_off(e, &in->src1), f32, opc))
      return false;
  } else
    return mach_err(e, "x64 machine form encode: unsupported float rhs");
  if (assigned >= 0) {
    if (e->fp_color_seeded)
      e->fp_color_seeded[in->dst.as.reg] = true;
    return true;
  }
  return mach_store_xmm(e, dst_off, f32, d);
}

/*
 * Reserve independent scalar colors and prove which ones survive each CFG
 * edge. Calls kill residency; definitions restore it. Homes remain current
 * so a path following a call can reload without depending on layout order.
 */
static void mach_allocate_loop_fprs(nyr_x64_mach_enc_t *e, const char *name) {
  const nyr_mach_func_t *m = e->mach;
  size_t nv = m->vreg_len, nb = m->block_len;
  if (!e->fp_fast_path || e->vec_fast_path || !nv || nv > 512 || nb > 128 ||
      !nb || getenv("NYTRIX_FORCE_STACK_FPR"))
    return;
  int *regs = malloc(nv * sizeof(*regs));
  size_t *score = calloc(nv, sizeof(*score));
  bool *eligible = malloc(nv * sizeof(*eligible));
  bool *edges = calloc(nb * nb, sizeof(*edges));
  uint8_t *entry = malloc(nb), *out = malloc(nb);
  if (!regs || !score || !eligible || !edges || !entry || !out)
    goto done;
  for (size_t v = 0; v < nv; ++v) {
    regs[v] = -1;
    eligible[v] = m->vreg_types[v] == NYR_MACH_TYPE_F64 ||
                  m->vreg_types[v] == NYR_MACH_TYPE_F32;
  }
  bool loop = false;
  for (size_t b = 0; b < nb; ++b) {
    const nyr_mach_block_t *block = &m->blocks[b];
    for (size_t n = 0; n < block->inst_count; ++n) {
      const nyr_mach_inst_t *in = &m->insts[block->first_inst + n];
      /* Conversions may use low XMM scratch registers. Model the clobber in
       * loop-home dataflow instead of disabling loop homes for the function. */
      if (in->opcode == NYR_MACH_SHUFFLE || in->opcode == NYR_MACH_FMA ||
          in->opcode == NYR_MACH_SQRT)
        goto done;
      if (in->src1.kind == NYR_MACH_OPERAND_BLOCK) {
        size_t target = in->src1.as.block_index;
        edges[b * nb + target] = true;
        loop |= target <= b;
      }
      if (in->dst.kind == NYR_MACH_OPERAND_VREG &&
          in->opcode != NYR_MACH_STORE) {
        size_t v = in->dst.as.reg;
        if (in->opcode != NYR_MACH_COPY && in->opcode != NYR_MACH_ADD &&
            in->opcode != NYR_MACH_SUB && in->opcode != NYR_MACH_MUL &&
            in->opcode != NYR_MACH_DIV)
          eligible[v] = false;
        if (in->opcode == NYR_MACH_COPY &&
            in->src0.kind == NYR_MACH_OPERAND_VREG &&
            !mach_is_float(m, &in->src0))
          eligible[v] = false;
      }
      const nyr_mach_operand_t *ops[] = {&in->src0, &in->src1, &in->src2};
      for (size_t k = 0; k < 3; ++k)
        if (ops[k]->kind == NYR_MACH_OPERAND_VREG)
          ++score[ops[k]->as.reg];
    }
    nyr_mach_opcode_t last =
        block->inst_count
            ? m->insts[block->first_inst + block->inst_count - 1].opcode
            : NYR_MACH_NOP;
    if (b + 1 < nb && last != NYR_MACH_BR && last != NYR_MACH_RET &&
        last != NYR_MACH_TRAP)
      edges[b * nb + b + 1] = true;
  }
  if (!loop)
    goto done;
  unsigned count = 0;
  for (; count < 6 && count < e->fp_color_limit; ++count) {
    size_t best = SIZE_MAX, weight = 0;
    for (size_t v = 0; v < nv; ++v)
      if (eligible[v] && regs[v] < 0 && score[v] > weight) {
        best = v;
        weight = score[v];
      }
    if (best == SIZE_MAX)
      break;
    regs[best] = (int)count;
    if (ny_dbg_is_enabled(NY_DBG_DEEP)) {
      ny_dbg_enter_context(name);
      ny_dbg_heuristic_event("X64_LOOP_FPR", "v%zu xmm%u uses=%zu", best, count,
                             weight);
      ny_dbg_exit_context(name);
    }
  }
  if (!count)
    goto done;
  /* Destructive scalar operations may reuse a dying left operand. Only
   * coalesce complete classes whose allocator intervals never overlap,
   * except at that instruction's read-before-write endpoint. */
  for (size_t i = 0; i < m->inst_len; ++i) {
    const nyr_mach_inst_t *in = &m->insts[i];
    if (in->dst.kind != NYR_MACH_OPERAND_VREG ||
        in->src0.kind != NYR_MACH_OPERAND_VREG ||
        (in->opcode != NYR_MACH_COPY && in->opcode != NYR_MACH_ADD &&
         in->opcode != NYR_MACH_SUB && in->opcode != NYR_MACH_MUL &&
         in->opcode != NYR_MACH_DIV))
      continue;
    int dst = regs[in->dst.as.reg], src = regs[in->src0.as.reg];
    if (dst < 0 || src < 0 || dst == src)
      continue;
    bool conflict = false;
    for (size_t x = 0; !conflict && x < e->fp_regalloc.segment_len; ++x) {
      const nyr_mach_live_segment_t *sx = &e->fp_regalloc.segments[x];
      if (regs[sx->vreg] != dst)
        continue;
      for (size_t y = 0; y < e->fp_regalloc.segment_len; ++y) {
        const nyr_mach_live_segment_t *sy = &e->fp_regalloc.segments[y];
        if (regs[sy->vreg] != src || sx->block != sy->block ||
            sx->start > sy->end || sy->start > sx->end)
          continue;
        size_t first = sx->start > sy->start ? sx->start : sy->start;
        size_t last = sx->end < sy->end ? sx->end : sy->end;
        const nyr_mach_inst_t *at = &m->insts[first];
        bool endpoint =
            first == last && at->dst.kind == NYR_MACH_OPERAND_VREG &&
            at->src0.kind == NYR_MACH_OPERAND_VREG &&
            ((at->dst.as.reg == sx->vreg && at->src0.as.reg == sy->vreg) ||
             (at->dst.as.reg == sy->vreg && at->src0.as.reg == sx->vreg)) &&
            (at->opcode == NYR_MACH_COPY || at->opcode == NYR_MACH_ADD ||
             at->opcode == NYR_MACH_SUB || at->opcode == NYR_MACH_MUL ||
             at->opcode == NYR_MACH_DIV);
        if (!endpoint) {
          conflict = true;
          break;
        }
      }
    }
    if (!conflict)
      for (size_t v = 0; v < nv; ++v)
        if (regs[v] == dst)
          regs[v] = src;
  }
  int dense[6] = {-1, -1, -1, -1, -1, -1};
  count = 0;
  for (size_t v = 0; v < nv; ++v)
    if (regs[v] >= 0) {
      if (dense[regs[v]] < 0)
        dense[regs[v]] = (int)count++;
      regs[v] = dense[regs[v]];
    }
  uint8_t full = (uint8_t)((1u << count) - 1);
  memset(entry, full, nb);
  memset(out, full, nb);
  bool changed;
  do {
    changed = false;
    for (size_t b = 0; b < nb; ++b) {
      uint8_t state = full;
      bool pred = false;
      for (size_t p = 0; p < nb; ++p)
        if (edges[p * nb + b]) {
          state &= out[p];
          pred = true;
        }
      if (!b || !pred)
        state = 0;
      entry[b] = state;
      const nyr_mach_block_t *block = &m->blocks[b];
      for (size_t n = 0; n < block->inst_count; ++n) {
        const nyr_mach_inst_t *in = &m->insts[block->first_inst + n];
        if (in->opcode == NYR_MACH_CALL)
          state = 0;
        if (in->dst.kind == NYR_MACH_OPERAND_VREG &&
            in->opcode != NYR_MACH_STORE && regs[in->dst.as.reg] >= 0)
          state |= (uint8_t)(1u << regs[in->dst.as.reg]);
        if (in->opcode == NYR_MACH_CONVERT)
          state = 0;
      }
      changed |= state != out[b];
      out[b] = state;
    }
  } while (changed);
  e->loop_fpr = regs;
  e->loop_fpr_entry = entry;
  regs = NULL;
  entry = NULL;
  for (size_t i = 0; i < e->fp_regalloc.segment_len; ++i) {
    nyr_mach_live_segment_t *s = &e->fp_regalloc.segments[i];
    int c = e->loop_fpr[s->vreg];
    if (c >= 0 || (s->color >= 0 && (unsigned)s->color < count)) {
      s->color = c;
      s->carried = s->acyclic_carried = false;
      s->reload = s->spill = true;
    }
  }
done:
  free(regs);
  free(score);
  free(eligible);
  free(edges);
  free(entry);
  free(out);
}

static bool mach_float_fast_eligible(const nyr_mach_func_t *mach) {
  if (!mach)
    return false;
  bool has_float = false;
  for (size_t i = 0; i < mach->inst_len; ++i) {
    const nyr_mach_inst_t *in = &mach->insts[i];
    const nyr_mach_operand_t *ops[] = {&in->dst, &in->src0, &in->src1,
                                       &in->src2};
    bool inst_float = false;
    for (size_t k = 0; k < sizeof(ops) / sizeof(ops[0]); ++k) {
      const nyr_mach_operand_t *op = ops[k];
      if (!op || op->kind != NYR_MACH_OPERAND_VREG ||
          op->as.reg >= mach->vreg_len)
        continue;
      nyr_mach_type_t t = mach->vreg_types[op->as.reg];
      if (t == NYR_MACH_TYPE_F32 || t == NYR_MACH_TYPE_F64)
        inst_float = true;
    }
    if (!inst_float)
      continue;
    has_float = true;
    if (in->opcode != NYR_MACH_COPY && in->opcode != NYR_MACH_ADD &&
        in->opcode != NYR_MACH_SUB && in->opcode != NYR_MACH_MUL &&
        in->opcode != NYR_MACH_DIV && in->opcode != NYR_MACH_FMA &&
        in->opcode != NYR_MACH_SQRT && in->opcode != NYR_MACH_SIN &&
        in->opcode != NYR_MACH_COS && in->opcode != NYR_MACH_CONVERT &&
        in->opcode != NYR_MACH_SHUFFLE && in->opcode != NYR_MACH_LOAD &&
        in->opcode != NYR_MACH_STORE && in->opcode != NYR_MACH_CMP &&
        in->opcode != NYR_MACH_BR && in->opcode != NYR_MACH_BR_IF &&
        in->opcode != NYR_MACH_NOP && in->opcode != NYR_MACH_RET)
      return false;
  }
  return has_float;
}

static bool mach_load_eax(nyr_x64_mach_enc_t *e, int off) {
  mach_rax_invalidate(e);
  return mach_u8(e, 0x8b) && mach_u8(e, 0x85) && mach_i32(e, off);
}

static bool mach_store_eax_rsp(nyr_x64_mach_enc_t *e, int off) {
  return mach_u8(e, 0x89) && mach_u8(e, 0x84) && mach_u8(e, 0x24) &&
         mach_i32(e, off);
}

static bool mach_store_rax_rsp(nyr_x64_mach_enc_t *e, int off) {
  return mach_rex(e, true, false, false, false) && mach_u8(e, 0x89) &&
         mach_u8(e, 0x84) && mach_u8(e, 0x24) && mach_i32(e, off);
}

static bool mach_load_rax_rsp(nyr_x64_mach_enc_t *e, int off) {
  mach_rax_invalidate(e);
  return mach_rex(e, true, false, false, false) && mach_u8(e, 0x8b) &&
         mach_u8(e, 0x84) && mach_u8(e, 0x24) && mach_i32(e, off);
}

static bool mach_param_slots(const nyr_mach_func_t *mach, bool *params) {
  if (!mach || (!params && mach->frame_slot_len))
    return false;
  bool *stored = calloc(mach->frame_slot_len, sizeof(*stored));
  if (mach->frame_slot_len && !stored)
    return false;
  size_t prefix = mach->param_count;
  if (prefix > mach->frame_slot_len) {
    free(stored);
    return false;
  }
  for (size_t i = 0; prefix == 0 && i < mach->inst_len; ++i) {
    const nyr_mach_inst_t *in = &mach->insts[i];
    if (in->opcode == NYR_MACH_STORE &&
        in->dst.kind == NYR_MACH_OPERAND_FRAME) {
      if (in->dst.as.frame_index >= mach->frame_slot_len) {
        free(stored);
        return false;
      }
      stored[in->dst.as.frame_index] = true;
    } else if (in->opcode == NYR_MACH_LOAD &&
               in->src0.kind == NYR_MACH_OPERAND_FRAME) {
      if (in->src0.as.frame_index >= mach->frame_slot_len) {
        free(stored);
        return false;
      }
      if (!stored[in->src0.as.frame_index] &&
          in->src0.as.frame_index + 1 > prefix)
        prefix = in->src0.as.frame_index + 1;
    }
  }
  for (size_t i = 0; i < prefix; ++i)
    params[i] = true;
  free(stored);
  return true;
}

static bool mach_store_gpr(nyr_x64_mach_enc_t *e, int off, int reg) {
  unsigned char modrm = (unsigned char)(0x85 | ((reg & 7) << 3));
  return mach_rex(e, true, reg >= 8, false, false) && mach_u8(e, 0x89) &&
         mach_u8(e, modrm) && mach_i32(e, off);
}

static bool mach_prepare_direct_params(nyr_x64_mach_enc_t *e) {
  const nyr_mach_func_t *mach = e->mach;
  bool trace = ny_dbg_is_enabled(NY_DBG_DEEP);
  if (!mach || !mach->param_count || mach->param_count > mach->frame_slot_len ||
      mach->param_aggregate_home_slots || !mach->block_len ||
      mach->param_count > 6) {
    if (trace && mach) {
      ny_dbg_enter_context(e->debug_name);
      ny_dbg_heuristic_event(
          "X64_PARAM_DIRECT",
          "skip params=%zu frames=%zu aggregate=%d blocks=%zu",
          mach->param_count, mach->frame_slot_len,
          mach->param_aggregate_home_slots != NULL, mach->block_len);
      ny_dbg_exit_context(e->debug_name);
    }
    return true;
  }

  int source_regs[6];
  size_t load_inst[6];
  int8_t direct_sources[6];
  bool direct_slots[6] = {false};
  for (size_t slot = 0; slot < mach->param_count; ++slot) {
    source_regs[slot] = -1;
    load_inst[slot] = SIZE_MAX;
    direct_sources[slot] = -1;
  }

  static const int gp_sysv[] = {7, 6, 2, 1, 8, 9};
  static const int gp_win[] = {1, 2, 8, 9};
  bool is_win = e->target && e->target->abi == NY_NATIVE_ABI_WIN64;
  const int *gp_regs = is_win ? gp_win : gp_sysv;
  size_t gp_arg = 0;
  for (size_t slot = 0; slot < mach->param_count; ++slot) {
    nyr_mach_type_t type = mach->frame_slots[slot].type;
    bool is_fp = type == NYR_MACH_TYPE_F32 || type == NYR_MACH_TYPE_F64;
    if (is_win) {
      if (!is_fp && e->target && slot < e->target->gp_arg_reg_count &&
          slot < sizeof(gp_win) / sizeof(gp_win[0]))
        source_regs[slot] = gp_regs[slot];
    } else {
      if (!is_fp && e->target && gp_arg < e->target->gp_arg_reg_count &&
          gp_arg < sizeof(gp_sysv) / sizeof(gp_sysv[0]))
        source_regs[slot] = gp_regs[gp_arg];
      if (!is_fp)
        ++gp_arg;
    }
  }

  for (size_t i = 0; i < mach->inst_len; ++i) {
    const nyr_mach_inst_t *in = &mach->insts[i];
    if (in->opcode == NYR_MACH_STORE &&
        in->dst.kind == NYR_MACH_OPERAND_FRAME &&
        in->dst.as.frame_index < mach->param_count) {
      if (trace) {
        ny_dbg_enter_context(e->debug_name);
        ny_dbg_heuristic_event("X64_PARAM_DIRECT",
                               "skip store-to-param slot=%u",
                               in->dst.as.frame_index);
        ny_dbg_exit_context(e->debug_name);
      }
      return true;
    }
    if (in->opcode != NYR_MACH_LOAD ||
        in->src0.kind != NYR_MACH_OPERAND_FRAME ||
        in->src0.as.frame_index >= mach->param_count)
      continue;
    size_t slot = in->src0.as.frame_index;
    if (load_inst[slot] != SIZE_MAX || in->dst.kind != NYR_MACH_OPERAND_VREG ||
        in->dst.as.reg >= mach->vreg_len || mach_is_float(mach, &in->dst)) {
      if (trace) {
        ny_dbg_enter_context(e->debug_name);
        ny_dbg_heuristic_event(
            "X64_PARAM_DIRECT", "skip load slot=%zu duplicate=%d dst-kind=%d",
            slot, load_inst[slot] != SIZE_MAX, (int)in->dst.kind);
        ny_dbg_exit_context(e->debug_name);
      }
      return true;
    }
    load_inst[slot] = i;
  }

  const nyr_mach_block_t *entry = &mach->blocks[0];
  if (entry->first_inst > mach->inst_len ||
      entry->inst_count > mach->inst_len - entry->first_inst) {
    if (trace) {
      ny_dbg_enter_context(e->debug_name);
      ny_dbg_heuristic_event("X64_PARAM_DIRECT", "skip malformed entry block");
      ny_dbg_exit_context(e->debug_name);
    }
    return true;
  }
  bool incoming_args_live = true;
  size_t prefix_end = entry->first_inst + entry->inst_count;
  for (size_t i = entry->first_inst; i < prefix_end; ++i) {
    const nyr_mach_inst_t *in = &mach->insts[i];
    /*
     * Parameter lowering commonly interleaves a LOAD with its local-home
     * STORE. A fixed frame store only reads a value and writes memory; it
     * leaves the remaining incoming GPR argument registers intact.
     */
    if (in->opcode == NYR_MACH_STORE &&
        in->dst.kind == NYR_MACH_OPERAND_FRAME &&
        in->dst.as.frame_index >= mach->param_count)
      continue;
    if (in->opcode != NYR_MACH_LOAD ||
        in->src0.kind != NYR_MACH_OPERAND_FRAME ||
        in->src0.as.frame_index >= mach->param_count ||
        load_inst[in->src0.as.frame_index] != i)
      break;

    size_t slot = in->src0.as.frame_index;
    size_t saved_inst = e->cur_inst;
    e->cur_inst = i;
    int preg = mach_vreg_def_preg(e, in->dst.as.reg);
    e->cur_inst = saved_inst;
    if (incoming_args_live && source_regs[slot] >= 0 && preg >= 0 &&
        (preg == 3 || (preg >= 12 && preg <= 15))) {
      direct_sources[slot] = (int8_t)source_regs[slot];
      direct_slots[slot] = true;
    }
    if (preg >= 0 && (preg == 7 || preg == 6 || preg == 2 || preg == 1 ||
                      preg == 8 || preg == 9))
      incoming_args_live = false;
  }
  size_t direct_count = 0;
  for (size_t slot = 0; slot < mach->param_count; ++slot)
    direct_count += direct_sources[slot] >= 0;
  if (direct_count) {
    int8_t *inst_sources =
        malloc((mach->inst_len ? mach->inst_len : 1) * sizeof(*inst_sources));
    bool *param_slots = calloc(mach->frame_slot_len, sizeof(*param_slots));
    if (!inst_sources || !param_slots) {
      free(inst_sources);
      free(param_slots);
      return mach_err(e, "x64 machine form encode: OOM (direct parameters)");
    }
    memset(inst_sources, -1, mach->inst_len * sizeof(*inst_sources));
    for (size_t slot = 0; slot < mach->param_count; ++slot)
      if (direct_sources[slot] >= 0)
        inst_sources[load_inst[slot]] = direct_sources[slot];
    memcpy(param_slots, direct_slots, mach->param_count * sizeof(*param_slots));
    e->direct_param_source = inst_sources;
    e->direct_param_slots = param_slots;
  }
  if (trace) {
    ny_dbg_enter_context(e->debug_name);
    ny_dbg_heuristic_event(
        "X64_PARAM_DIRECT", "params=%zu entry-prefix=%zu direct=%zu",
        mach->param_count, prefix_end - entry->first_inst, direct_count);
    ny_dbg_exit_context(e->debug_name);
  }
  return true;
}

static bool mach_spill_params(nyr_x64_mach_enc_t *e) {
  const nyr_mach_func_t *mach = e->mach;
  if (!mach->frame_slot_len)
    return true;
  bool *params = calloc(mach->frame_slot_len, sizeof(*params));
  if (!params)
    return mach_err(e, "x64 machine form encode: OOM");
  if (!mach_param_slots(mach, params)) {
    free(params);
    return mach_err(e, "x64 machine form encode: invalid parameter slots");
  }
  static const int gp_sysv[] = {7, 6, 2, 1, 8, 9};
  static const int gp_win[] = {1, 2, 8, 9};
  bool is_win = e->target && e->target->abi == NY_NATIVE_ABI_WIN64;
  const int *gp_regs = is_win ? gp_win : gp_sysv;
  size_t gp_arg = 0, fp_arg = 0, ordinal = 0, stack_arg = 0;
  for (size_t slot = 0; slot < mach->frame_slot_len; ++slot) {
    if (!params[slot])
      continue;
    bool is_f32 = mach->frame_slots[slot].type == NYR_MACH_TYPE_F32;
    bool is_f64 = mach->frame_slots[slot].type == NYR_MACH_TYPE_F64;
    bool is_fp = is_f32 || is_f64;
    bool from_stack = is_win  ? ordinal >= e->target->gp_arg_reg_count
                      : is_fp ? fp_arg >= e->target->fp_arg_reg_count
                              : gp_arg >= e->target->gp_arg_reg_count;
    int dst = -8 * (e->cs_slots + (int)slot + 1);
    bool direct = e->direct_param_slots && e->direct_param_slots[slot];
    if (direct) {
      if (ny_dbg_is_enabled(NY_DBG_DEEP)) {
        ny_dbg_enter_context(e->debug_name);
        ny_dbg_heuristic_event("X64_PARAM_DIRECT", "skip home slot=%zu", slot);
        ny_dbg_exit_context(e->debug_name);
      }
      /* The entry LOAD copies this argument straight into its assigned preg. */
    } else if (from_stack) {
      int incoming = is_win ? 16 + (int)ordinal * 8 : 16 + (int)stack_arg * 8;
      if (is_fp) {
        if (!mach_load_xmm0(e, incoming, is_f32) ||
            !mach_store_xmm0(e, dst, is_f32)) {
          free(params);
          return false;
        }
      } else if (!mach_load_rax(e, incoming) || !mach_store_rax(e, dst)) {
        free(params);
        return false;
      }
      ++stack_arg;
    } else if (is_fp) {
      size_t index = is_win ? ordinal : fp_arg;
      if (!mach_store_xmm(e, dst, is_f32, (unsigned)index)) {
        free(params);
        return false;
      }
    } else {
      size_t index = is_win ? ordinal : gp_arg;
      if (!mach_store_gpr(e, dst, gp_regs[index])) {
        free(params);
        return false;
      }
    }
    if (is_win)
      ++ordinal;
    else if (is_fp)
      ++fp_arg;
    else
      ++gp_arg;
  }
  free(params);
  mach_rax_invalidate(e);
  return true;
}

/*
 * SSE scalar op xmm0, mem: F2/F3 0F opc 85 disp — opc: 58 add 5c sub 59 mul
 * 5e div
 */
static bool mach_sse_op_mem(nyr_x64_mach_enc_t *e, int off, bool f32,
                            unsigned opc) {
  return mach_u8(e, f32 ? 0xf3 : 0xf2) && mach_u8(e, 0x0f) && mach_u8(e, opc) &&
         mach_u8(e, 0x85) && mach_i32(e, off);
}

static bool ny_mach_profile_hot_loop_block(const nyr_mach_func_t *mach,
                                           uint32_t block) {
  if (!mach || block >= mach->block_len)
    return false;
  uint32_t source_pc = mach->blocks[block].source_pc;
  return source_pc != UINT32_MAX && ny_native_profile_loop_hot(source_pc) >= 16;
}

static size_t ny_mach_regalloc_hot_peak_live(const nyr_mach_func_t *mach,
                                             const nyr_mach_regalloc_t *a,
                                             size_t inst_len) {
  if (!mach || !a || !inst_len)
    return 0;
  int *delta = calloc(inst_len + 1, sizeof(*delta));
  if (!delta)
    return 0;
  for (size_t i = 0; i < a->segment_len; ++i) {
    const nyr_mach_live_segment_t *seg = &a->segments[i];
    if (!ny_mach_profile_hot_loop_block(mach, seg->block) ||
        seg->start >= inst_len)
      continue;
    size_t end = seg->end < inst_len ? seg->end : inst_len - 1;
    ++delta[seg->start];
    if (end + 1 < inst_len)
      --delta[end + 1];
  }
  size_t peak = 0;
  int live = 0;
  for (size_t pc = 0; pc < inst_len; ++pc) {
    live += delta[pc];
    if (live > 0 && (size_t)live > peak)
      peak = (size_t)live;
  }
  free(delta);
  return peak;
}

static void ny_mach_regalloc_summarize(const nyr_mach_func_t *mach,
                                       const nyr_mach_regalloc_t *a,
                                       size_t inst_len,
                                       nyr_native_regalloc_metrics_t *out) {
  if (!out)
    return;
  *out = (nyr_native_regalloc_metrics_t){0};
  if (!a)
    return;
  out->segments = a->segment_len;
  for (size_t i = 0; i < a->segment_len; ++i) {
    const nyr_mach_live_segment_t *seg = &a->segments[i];
    if (seg->color >= 0)
      ++out->colored;
    else
      ++out->spilled;
    if (seg->reload)
      ++out->reloads;
    if (!ny_mach_profile_hot_loop_block(mach, seg->block))
      continue;
    ++out->hot_loop_segments;
    if (seg->color < 0)
      ++out->hot_loop_spilled;
    if (seg->reload)
      ++out->hot_loop_reloads;
  }
  out->peak_live = nyr_mach_regalloc_peak_live(a, inst_len);
  out->hot_loop_peak_live = ny_mach_regalloc_hot_peak_live(mach, a, inst_len);
}

bool nyr_native_x64_regalloc_metrics(const nyr_mach_func_t *mach,
                                     const ny_native_target_info_t *target,
                                     nyr_native_regalloc_metrics_t *gpr,
                                     nyr_native_regalloc_metrics_t *fpr,
                                     nyr_native_regalloc_metrics_t *vector) {
  if (gpr)
    *gpr = (nyr_native_regalloc_metrics_t){0};
  if (fpr)
    *fpr = (nyr_native_regalloc_metrics_t){0};
  if (vector)
    *vector = (nyr_native_regalloc_metrics_t){0};
  if (!mach)
    return false;

  size_t gp_color_count = mach_gpr_color_count(mach, target);

  nyr_mach_regalloc_t a = {0};
  if (mach->vreg_len &&
      !nyr_mach_regalloc_build_callee_saved_gpr(mach, gp_color_count, &a))
    return false;
  ny_mach_regalloc_summarize(mach, &a, mach->inst_len, gpr);
  nyr_mach_regalloc_free(&a);

  bool fp_eligible = mach_float_fast_eligible(mach);
  for (size_t i = 0; fp_eligible && i < mach->inst_len; ++i)
    if (mach->insts[i].opcode == NYR_MACH_SIN ||
        mach->insts[i].opcode == NYR_MACH_COS)
      fp_eligible = false;
  if (mach->vreg_len && fp_eligible) {
    if (!nyr_mach_regalloc_build_class(mach, NYR_MACH_REGCLASS_FPR, MFP_COLOR_N,
                                       &a)) {
      nyr_mach_regalloc_free(&a);
      return false;
    }
    ny_mach_regalloc_summarize(mach, &a, mach->inst_len, fpr);
    nyr_mach_regalloc_free(&a);
  }

  if (mach->vreg_len && mach_vector_fast_eligible(mach)) {
    if (!nyr_mach_regalloc_build_class(mach, NYR_MACH_REGCLASS_VECTOR,
                                       MVEC_COLOR_N, &a)) {
      nyr_mach_regalloc_free(&a);
      return false;
    }
    ny_mach_regalloc_summarize(mach, &a, mach->inst_len, vector);
    nyr_mach_regalloc_free(&a);
  }
  return true;
}

static bool mach_encode_function(nyr_x64_mach_enc_t *e, const char *name,
                                 bool tag_return) {
  (void)tag_return;
  e->debug_name = name;
  const nyr_mach_func_t *mach = e->mach;
  e->inst_offsets =
      calloc(mach->inst_len ? mach->inst_len : 1, sizeof(*e->inst_offsets));
  if (!e->inst_offsets && mach->inst_len)
    return mach_err(e, "x64 machine form encode: OOM (source map)");
  for (size_t i = 0; i < mach->inst_len; ++i)
    e->inst_offsets[i] = SIZE_MAX;
  e->vec_fast_path = mach_vector_fast_eligible(mach);
  e->fp_fast_path = mach_float_fast_eligible(mach);
  for (size_t i = 0; e->fp_fast_path && i < mach->inst_len; ++i)
    if (mach->insts[i].opcode == NYR_MACH_SIN ||
        mach->insts[i].opcode == NYR_MACH_COS)
      e->fp_fast_path = false;
  /*
   * The four-lane f64 shuffle slow path uses xmm0-xmm3 as scratch while
   * scalar float colors span xmm0-xmm13; the two fast paths must not
   * coexist in one function.
   */
  for (size_t i = 0; e->fp_fast_path && i < mach->inst_len; ++i)
    if (mach->insts[i].opcode == NYR_MACH_SHUFFLE &&
        mach->insts[i].dst.kind == NYR_MACH_OPERAND_VREG &&
        mach->insts[i].dst.as.reg < mach->vreg_len &&
        mach->vreg_types[mach->insts[i].dst.as.reg] == NYR_MACH_TYPE_V256_F64)
      e->fp_fast_path = false;
  /*
   * When vector values exist but the vector fast path is unavailable, the
   * vector encoders fall back to xmm0-xmm3 scratch sequences. Those would
   * clobber seeded scalar float colors, so scalar float residency yields to
   * any slow-path vector lowering (with the vector fast path active, vector
   * colors live in xmm8+ and cannot collide with scalar float colors).
   */
  if (!e->vec_fast_path && mach_vector_types_present(mach))
    e->fp_fast_path = false;
  /*
   * Scalar float colors are clamped below the vector XMM half whenever the
   * vector fast path is active; see MVEC_PREG_BASE.
   */
  e->fp_color_limit = e->vec_fast_path ? 8 : MFP_COLOR_N;
  e->last_rax_valid = false;
  e->last_rax_off = 0;
  unsigned save_mask = 0;
  e->regalloc = (nyr_mach_regalloc_t){0};
  e->fp_regalloc = (nyr_mach_regalloc_t){0};
  e->vec_regalloc = (nyr_mach_regalloc_t){0};
  e->color_seeded = NULL;
  e->fp_color_seeded = NULL;
  e->vec_color_seeded = NULL;
  e->cur_inst = 0;
  e->cur_block = 0;
  e->cur_block_valid = false;
  /*
   * Sticky rax is a within-block cache: a block with multiple predecessors
   * can be reached with different rax contents, so the cache cannot survive
   * a control-flow edge.  mach_encode_function invalidates rax at every
   * block entry below, which makes the store->load elision sound for
   * multi-block functions (loops) too.
   */
  e->no_sticky = false;
  e->fused_jcc = 0;
  bool color_seeded_stack[MACH_SEEDED_STACK_MAX];
  bool fp_color_seeded_stack[MACH_SEEDED_STACK_MAX];
  bool vec_color_seeded_stack[MACH_SEEDED_STACK_MAX];
  int64_t *imm_fold = NULL;
  bool *imm_fold_known = NULL;
  nyr_mach_operand_t *addr_fold = NULL;
  bool *addr_fold_known = NULL;
  /*
   * Caller-saved colors are reserved in call blocks; bounded local segments
   * in other blocks may use them.
   */
  size_t gp_color_count = mach_gpr_color_count(mach, e->target);
  e->colors_len = mach->vreg_len;
  const char *dump_regalloc = getenv("NY_DUMP_REGALLOC");
  bool dump_this_function =
      dump_regalloc && (strcmp(dump_regalloc, "1") == 0 ||
                        (name && strcmp(dump_regalloc, name) == 0));
  if (mach->vreg_len) {
    if (mach->vreg_len <= MACH_SEEDED_STACK_MAX) {
      e->color_seeded = color_seeded_stack;
      memset(e->color_seeded, 0, mach->vreg_len * sizeof(bool));
    } else {
      e->color_seeded = calloc(mach->vreg_len, sizeof(*e->color_seeded));
    }
    e->cmp_elision_action =
        calloc(mach->vreg_len, sizeof(*e->cmp_elision_action));
    if (!e->color_seeded || !e->cmp_elision_action)
      return mach_err(e, "x64 machine form encode: OOM");
    if (!mach_prepare_cmp_flag_reuse(e, name))
      return mach_err(e,
                      "x64 machine form encode: compare reuse analysis failed");
    nyr_mach_func_t alloc_view;
    nyr_mach_inst_t *alloc_inst_copy = NULL;
    if (!mach_cmp_reuse_alloc_view(e, &alloc_view, &alloc_inst_copy))
      return mach_err(e, "x64 machine form encode: compare reuse view failed");
    const nyr_mach_func_t *saved_mach = e->mach;
    if (alloc_inst_copy)
      e->mach = &alloc_view;
    bool alloc_ok = nyr_mach_regalloc_build_callee_saved_gpr(
        e->mach, gp_color_count, &e->regalloc);
    if (alloc_ok)
      mach_allocate_loop_gprs(e, name);
    e->mach = saved_mach;
    free(alloc_inst_copy);
    if (!alloc_ok) {
      if (e->color_seeded && e->color_seeded != color_seeded_stack)
        free(e->color_seeded);
      e->color_seeded = NULL;
      nyr_mach_regalloc_free(&e->regalloc);
      return mach_err(e, "x64 machine form encode: register allocation failed");
    }
    if (e->loop_gpr)
      for (size_t v = 0; v < e->colors_len; ++v)
        if (e->loop_gpr[v] >= 0 && e->loop_gpr[v] < MIR_CALLEE_COLOR_N)
          save_mask |= 1u << e->loop_gpr[v];
    for (size_t i = 0; i < e->regalloc.segment_len; ++i) {
      int c = e->regalloc.segments[i].color;
      if (c >= 0 && c < MIR_CALLEE_COLOR_N)
        save_mask |= 1u << c;
    }
    if (dump_this_function) {
      fprintf(stderr, "[x64-regalloc] %s vregs=%zu segments=%zu\n",
              name ? name : "fn", mach->vreg_len, e->regalloc.segment_len);
      for (size_t i = 0; i < e->regalloc.segment_len; ++i) {
        const nyr_mach_live_segment_t *seg = &e->regalloc.segments[i];
        const char *spill_reason = "none";
        switch (seg->spill_reason) {
        case NYR_MACH_SPILL_FIXED_CLOBBER:
          spill_reason = "fixed-clobber";
          break;
        case NYR_MACH_SPILL_LIVE_OUT:
          spill_reason = "live-out";
          break;
        case NYR_MACH_SPILL_SEGMENT_BOUND:
          spill_reason = "segment-bound";
          break;
        case NYR_MACH_SPILL_REGISTER_PRESSURE:
          spill_reason = "register-pressure";
          break;
        case NYR_MACH_SPILL_BLOCK_CONFLICT:
          spill_reason = "block-conflict";
          break;
        case NYR_MACH_SPILL_NONE:
          break;
        }
        fprintf(
            stderr,
            "  v%u b%u [%zu,%zu] color=%d reload=%d spill=%d carried=%d "
            "live=%d→%d reason=%s\n",
            seg->vreg, seg->block, seg->start, seg->end, seg->color,
            (int)seg->reload, (int)seg->spill, (int)seg->carried,
            (int)nyr_mach_regalloc_live_in(&e->regalloc, seg->block, seg->vreg),
            (int)nyr_mach_regalloc_live_out(&e->regalloc, seg->block,
                                            seg->vreg),
            spill_reason);
      }
      nyr_mach_dump(mach, stderr);
    }
  }
  e->fp_colors_len = mach->vreg_len;
  if (e->fp_fast_path && mach->vreg_len) {
    if (mach->vreg_len <= MACH_SEEDED_STACK_MAX) {
      e->fp_color_seeded = fp_color_seeded_stack;
      memset(e->fp_color_seeded, 0, mach->vreg_len * sizeof(bool));
    } else {
      e->fp_color_seeded = calloc(mach->vreg_len, sizeof(*e->fp_color_seeded));
    }
    if (!e->fp_color_seeded ||
        !nyr_mach_regalloc_build_class(mach, NYR_MACH_REGCLASS_FPR,
                                       e->fp_color_limit, &e->fp_regalloc)) {
      if (e->fp_color_seeded && e->fp_color_seeded != fp_color_seeded_stack)
        free(e->fp_color_seeded);
      e->fp_color_seeded = NULL;
      nyr_mach_regalloc_free(&e->fp_regalloc);
      e->fp_fast_path = false;
    }
  }
  mach_allocate_loop_fprs(e, name);
  e->vec_colors_len = mach->vreg_len;
  if (e->vec_fast_path && mach->vreg_len) {
    if (mach->vreg_len <= MACH_SEEDED_STACK_MAX) {
      e->vec_color_seeded = vec_color_seeded_stack;
      memset(e->vec_color_seeded, 0, mach->vreg_len * sizeof(bool));
    } else {
      e->vec_color_seeded =
          calloc(mach->vreg_len, sizeof(*e->vec_color_seeded));
    }
    if (!e->vec_color_seeded ||
        !nyr_mach_regalloc_build_class(mach, NYR_MACH_REGCLASS_VECTOR,
                                       MVEC_COLOR_N, &e->vec_regalloc)) {
      if (e->vec_color_seeded && e->vec_color_seeded != vec_color_seeded_stack)
        free(e->vec_color_seeded);
      e->vec_color_seeded = NULL;
      nyr_mach_regalloc_free(&e->vec_regalloc);
      e->vec_fast_path = false;
    }
  }
  /*
   * Vector scalar scratch clobbers are represented in the GPR allocator;
   * vector values still use their separate XMM/YMM color bank.
   * Calls and fixed-register sequences publish resident scalar values to
   * canonical homes and invalidate their residency before clobbering.
   */
  size_t call_count = 0;
  bool has_wide_call = false;
  for (size_t i = 0; i < mach->inst_len; ++i)
    if (mach->insts[i].opcode == NYR_MACH_CALL) {
      ++call_count;
      has_wide_call |= mach->insts[i].args_len > 6;
    }
  /*
   * Keep canonical GPR homes authoritative around stack-argument calls. The
   * machine call lowering reloads argument operands from homes, and resident
   * call-result values are not yet proven to have a current home on every
   * path. CFGs above the loop allocator's 128-block proof limit also stay on
   * homes; smaller CFGs use its bounded edge and liveness proofs.
   */
  bool env_forces_stack = getenv("NYTRIX_FORCE_STACK_GPR") != NULL;
  const char *stack_reason = env_forces_stack        ? "environment"
                             : has_wide_call         ? "stack-arguments"
                             : call_count > 32       ? "call-count"
                             : mach->block_len > 128 ? "block-count"
                             : mach->vreg_len > 640  ? "value-count"
                                                     : "none";
  e->force_stack_reads = env_forces_stack || has_wide_call || call_count > 32 ||
                         mach->block_len > 128 || mach->vreg_len > 640;
  if (ny_dbg_is_enabled(NY_DBG_HEURISTIC) && ny_dbg_function_selected(name)) {
    ny_dbg_enter_context(name ? name : "<unknown>");
    ny_dbg_heuristic_event(
        "GPR_HOME",
        "function=%s force-stack=%s reason=%s calls=%zu blocks=%zu "
        "values=%zu segments=%zu loop-registers=%s save-mask=0x%x",
        name ? name : "<unknown>", e->force_stack_reads ? "yes" : "no",
        stack_reason, call_count, mach->block_len, mach->vreg_len,
        e->regalloc.segment_len, e->loop_gpr ? "selected" : "none", save_mask);
    ny_dbg_exit_context(name ? name : "<unknown>");
  }
  /*
   * Loop-edge carry coalescing is opt-in until the encoder's edge proof is
   * sound for every carried value. The explicit opt-in is shared with the
   * allocator; without it, keep canonical homes authoritative at block entry.
   */
  const char *loop_carry_env = getenv("NYTRIX_GPR_LOOP_CARRY");
  e->loop_carry_seeding = loop_carry_env && strcmp(loop_carry_env, "0") != 0 &&
                          strcmp(loop_carry_env, "off") != 0 &&
                          strcmp(loop_carry_env, "false") != 0;

  /*
   * One-time constant map: NYIR CONST_I64 lowers to COPY vreg <- IMM. COPY
   * chains preserve that value identity across block layout and compaction,
   * so integer consumers can fold the final vreg without reading its home.
   */
  if (mach->vreg_len) {
    imm_fold = calloc(mach->vreg_len, sizeof(*imm_fold));
    imm_fold_known = calloc(mach->vreg_len, sizeof(*imm_fold_known));
    addr_fold = calloc(mach->vreg_len, sizeof(*addr_fold));
    addr_fold_known = calloc(mach->vreg_len, sizeof(*addr_fold_known));
    if (!imm_fold || !imm_fold_known || !addr_fold || !addr_fold_known) {
      free(imm_fold);
      free(imm_fold_known);
      free(addr_fold);
      free(addr_fold_known);
      return mach_err(e,
                      "x64 machine form encode: OOM (rematerialization map)");
    }
    if (!mach_build_imm_fold(mach, imm_fold, imm_fold_known)) {
      free(imm_fold);
      free(imm_fold_known);
      free(addr_fold);
      free(addr_fold_known);
      return mach_err(e, "x64 machine form encode: OOM (constant map)");
    }
    /*
     * Address rematerialization is entry-block-only for the same
     * dominance reason as the constant map above.
     */
    if (mach->block_len > 0 && mach->inst_len > 0) {
      const nyr_mach_block_t *aentry = &mach->blocks[0];
      const size_t aend = aentry->first_inst + aentry->inst_count;
      for (size_t i = aentry->first_inst; i < aend && i < mach->inst_len; ++i) {
        const nyr_mach_inst_t *ci = &mach->insts[i];
        if (ci->opcode == NYR_MACH_LEA &&
            ci->dst.kind == NYR_MACH_OPERAND_VREG &&
            ci->dst.as.reg < mach->vreg_len &&
            (ci->src0.kind == NYR_MACH_OPERAND_FRAME ||
             ci->src0.kind == NYR_MACH_OPERAND_SYMBOL)) {
          addr_fold[ci->dst.as.reg] = ci->src0;
          addr_fold_known[ci->dst.as.reg] = true;
        }
      }
    }
  }
  e->imm_fold = imm_fold;
  e->imm_fold_known = imm_fold_known;
  e->addr_fold = addr_fold;
  e->addr_fold_known = addr_fold_known;
  e->save_mask = save_mask;
  e->cs_slots = mach_cs_count(save_mask);
  /*
   * Frame still reserves stack for spills and frame slots; colored vregs
   * may avoid loads when resident. Callee-save pushes already reserve
   * cs_slots*8 below %rbp, so sub only covers frame+vreg homes. Keep
   * (cs_slots + frame/8) even so %rsp is 16-byte aligned at calls.
   *
   * Frame slots are typed (I64=8, V128=16, V256=32); walk real sizes.
   */
  size_t frame_slots_bytes = 0;
  for (size_t slot = 0; slot < mach->frame_slot_len; ++slot) {
    size_t ssz = mach->frame_slots[slot].size;
    if (ssz < 8)
      ssz = 8;
    size_t salign = mach->frame_slots[slot].align;
    if (salign < 1)
      salign = 1;
    frame_slots_bytes = (frame_slots_bytes + salign - 1) & ~(salign - 1);
    frame_slots_bytes += ssz;
  }
  if (!mach_build_vreg_homes(e, frame_slots_bytes) ||
      frame_slots_bytes > SIZE_MAX - e->vreg_home_bytes)
    return mach_err(e, "x64 machine form encode: frame size overflow");
  size_t frame_bytes = frame_slots_bytes + e->vreg_home_bytes;
  if (ny_dbg_is_enabled(NY_DBG_DEEP)) {
    size_t spilled = 0, reloads = 0, hot_spilled = 0, hot_reloads = 0;
    for (size_t i = 0; i < e->regalloc.segment_len; ++i) {
      const nyr_mach_live_segment_t *seg = &e->regalloc.segments[i];
      spilled += seg->color < 0;
      reloads += seg->reload;
      if (ny_mach_profile_hot_loop_block(mach, seg->block)) {
        hot_spilled += seg->color < 0;
        hot_reloads += seg->reload;
      }
    }
    ny_dbg_heuristic_event(
        "X64_FRAME",
        "function=%s blocks=%zu insts=%zu vregs=%zu gpr_segments=%zu "
        "gpr_spills=%zu gpr_reloads=%zu hot_loop_spills=%zu "
        "hot_loop_reloads=%zu frame_slots=%zu vreg_homes=%zu "
        "frame_bytes=%zu",
        name ? name : "<anonymous>", mach->block_len, mach->inst_len,
        mach->vreg_len, e->regalloc.segment_len, spilled, reloads, hot_spilled,
        hot_reloads, frame_slots_bytes, e->vreg_home_bytes, frame_bytes);
  }
  if (dump_this_function) {
    fprintf(stderr,
            "[x64-frame] %s frame_slots=%zu callee_save_slots=%d "
            "vreg_homes=%zu frame_bytes=%zu aligned_frame=%zu\n",
            name ? name : "fn", frame_slots_bytes, e->cs_slots,
            e->vreg_home_bytes, frame_bytes,
            (frame_bytes + 15u) & ~(size_t)15u);
    for (size_t vreg = 0; vreg < mach->vreg_len; ++vreg)
      fprintf(stderr, "  [x64-home] v%zu offset=%d size=%zu\n", vreg,
              mach_vreg_home_off(e, (uint32_t)vreg),
              mach_vreg_home_size(mach, vreg));
    for (size_t i = 0; i < e->fp_regalloc.segment_len; ++i) {
      const nyr_mach_live_segment_t *seg = &e->fp_regalloc.segments[i];
      fprintf(stderr, "  [x64-fpreg] v%u b%u [%zu,%zu] color=%d spill=%d\n",
              seg->vreg, seg->block, seg->start, seg->end, seg->color,
              (int)seg->spill);
    }
    for (size_t i = 0; i < e->vec_regalloc.segment_len; ++i) {
      const nyr_mach_live_segment_t *seg = &e->vec_regalloc.segments[i];
      fprintf(stderr,
              "  [x64-vecreg] v%u b%u [%zu,%zu] color=%d spill=%d "
              "reason=%d\n",
              seg->vreg, seg->block, seg->start, seg->end, seg->color,
              (int)seg->spill, (int)seg->spill_reason);
    }
  }
  if (frame_bytes > (size_t)INT_MAX - 23u)
    return mach_err(e, "x64 machine form encode: frame too large");
  int frame = (int)((frame_bytes + 15u) & ~(size_t)15u);
  if ((e->cs_slots & 1) != 0)
    frame += 8;
  if (((size_t)e->cs_slots * 8u + (size_t)frame) % 16u != 0)
    return mach_err(e,
                    "x64 machine form encode: internal stack alignment error");

  /*
   * push %rbp; mov %rsp,%rbp
   */
  if (!mach_u8(e, 0x55) || !mach_rex(e, true, false, false, false) ||
      !mach_u8(e, 0x89) || !mach_u8(e, 0xe5))
    return false;
  if (!mach_push_callee_saves(e, save_mask))
    return false;
  if (frame) {
    /*
     * sub $frame, %rsp
     */
    if (!mach_rex(e, true, false, false, false) || !mach_u8(e, 0x81) ||
        !mach_u8(e, 0xec) || !mach_i32(e, frame))
      return false;
  }
  e->last_rax_valid = false;

  const char *base_name = name;
  const char *symbol_prefix =
      e->target && e->target->symbol_prefix ? e->target->symbol_prefix : "";
  size_t symbol_prefix_len = strlen(symbol_prefix);
  if (base_name && symbol_prefix_len &&
      strncmp(base_name, symbol_prefix, symbol_prefix_len) == 0)
    base_name += symbol_prefix_len;
  if (base_name && strcmp(base_name, "main") == 0 && e->target &&
      e->target->abi == NY_NATIVE_ABI_SYSV) {
    char set_args[256];
    snprintf(set_args, sizeof(set_args), "%s_ny_aot_set_args", symbol_prefix);
    if (!mach_u8(e, 0xe8) || !mach_add_reloc(e, set_args, NY_RELOC_PLT32) ||
        !mach_i32(e, 0))
      return false;
  }
  if (base_name && strcmp(base_name, "rt_main") != 0 &&
      strcmp(base_name, "main") != 0 && !mach_prepare_direct_params(e))
    return false;
  if (base_name && strcmp(base_name, "rt_main") != 0 &&
      strcmp(base_name, "main") != 0 && !mach_spill_params(e))
    return false;

  if (mach->vreg_len) {
    e->def_inst = malloc(mach->vreg_len * sizeof(*e->def_inst));
    e->select_test_src = malloc(mach->vreg_len * sizeof(*e->select_test_src));
    e->select_test_equal =
        calloc(mach->vreg_len, sizeof(*e->select_test_equal));
    e->select_skip_and = calloc(mach->vreg_len, sizeof(*e->select_skip_and));
    if (!e->def_inst || !e->select_test_src || !e->select_test_equal ||
        !e->cmp_elision_action || !e->select_skip_and)
      return mach_err(e, "x64 machine form encode: OOM");
    for (size_t v = 0; v < mach->vreg_len; ++v) {
      e->def_inst[v] = SIZE_MAX;
      e->select_test_src[v] = -1;
    }
    for (size_t i = 0; i < mach->inst_len; ++i) {
      const nyr_mach_inst_t *mi = &mach->insts[i];
      if (mi->opcode != NYR_MACH_STORE &&
          mi->dst.kind == NYR_MACH_OPERAND_VREG &&
          mi->dst.as.reg < mach->vreg_len)
        e->def_inst[mi->dst.as.reg] = i;
    }
    if (!mach_prepare_select_bit_test(e, name))
      return mach_err(e, "x64 machine form encode: select fusion failed");
  }
  if (!mach_build_vreg_block_use(e))
    return false;
  if (!mach_build_literal_use_map(e))
    return mach_err(e, "x64 machine form encode: OOM (literal use map)");
  e->block_off = calloc(mach->block_len ? mach->block_len : 1, sizeof(size_t));
  if (!e->block_off && mach->block_len)
    return mach_err(e, "x64 machine form encode: OOM");
  for (size_t bi = 0; bi < mach->block_len; ++bi) {
    e->cur_block = bi;
    e->cur_block_valid = true;
    e->block_off[bi] = e->code.len;
    const nyr_mach_block_t *blk = &mach->blocks[bi];
    /*
     * Invalidate the sticky-rax cache on every control-flow edge: rax may
     * hold any predecessor's last-loaded slot here, and claiming it holds
     * this block's first slot would elide a needed load.
     */
    mach_rax_invalidate(e);
    e->pending_sib_valid = false;
    mach_r10_index_invalidate(e);
    e->fused_jcc = 0; /* no flags-only CMP pending across a block edge */
    e->cur_inst = blk->first_inst;
    /*
     * Ordinary register residency is a runtime-predecessor property. Carried
     * segments are the exception: loop-edge coalescing proved that every
     * incoming edge supplies the same physical register.
     * Apply the operand-read eligibility guard here as well: a cross-block
     * value read from its home has no resident register to flush. Seeding
     * that value would let a later barrier overwrite its home with another
     * value occupying the allocator's nominal color.
     */
    if (e->color_seeded && e->colors_len)
      memset(e->color_seeded, 0, e->colors_len * sizeof(*e->color_seeded));
    if (e->loop_gpr)
      for (uint32_t v = 0; v < e->colors_len; ++v)
        if (e->loop_gpr[v] >= 0 &&
            nyr_mach_regalloc_live_in(&e->regalloc, bi, v))
          e->color_seeded[v] = true;
    if (e->fp_color_seeded && e->fp_colors_len) {
      memset(e->fp_color_seeded, 0,
             e->fp_colors_len * sizeof(*e->fp_color_seeded));
      if (e->loop_carry_seeding)
        for (size_t si = 0; si < e->fp_regalloc.segment_len; ++si) {
          const nyr_mach_live_segment_t *seg = &e->fp_regalloc.segments[si];
          if (seg->block == bi && seg->carried && !seg->spill &&
              seg->color >= 0 && seg->vreg < e->fp_colors_len)
            e->fp_color_seeded[seg->vreg] = true;
        }
    }
    if (e->vec_color_seeded && e->vec_colors_len) {
      memset(e->vec_color_seeded, 0,
             e->vec_colors_len * sizeof(*e->vec_color_seeded));
      if (e->loop_carry_seeding)
        for (size_t si = 0; si < e->vec_regalloc.segment_len; ++si) {
          const nyr_mach_live_segment_t *seg = &e->vec_regalloc.segments[si];
          if (seg->block == bi && seg->carried && !seg->spill &&
              seg->color >= 0 && seg->vreg < e->vec_colors_len)
            e->vec_color_seeded[seg->vreg] = true;
        }
    }
    if (e->loop_fpr)
      for (uint32_t v = 0; v < e->fp_colors_len; ++v)
        if (e->loop_fpr[v] >= 0)
          e->fp_color_seeded[v] =
              nyr_mach_regalloc_live_in(&e->fp_regalloc, bi, v) &&
              (e->loop_fpr_entry[bi] & (1u << e->loop_fpr[v])) != 0;
    for (uint32_t v = 0; v < e->colors_len; ++v) {
      const nyr_mach_live_segment_t *seg =
          mach_segment_at(e, v, blk->first_inst);
      bool allow_loop_carry = e->loop_carry_seeding && seg && seg->carried;
      if (seg && (seg->acyclic_carried || allow_loop_carry) &&
          seg->color >= 0 && mach_vreg_assigned_preg(e, v) >= 0)
        e->color_seeded[v] = true;
    }
    for (size_t n = 0; n < blk->inst_count; ++n) {
      e->cur_inst = blk->first_inst + n;
      e->pending_rax_valid = false;
      e->inst_offsets[e->cur_inst] = e->code.len;
      const nyr_mach_inst_t *in = &mach->insts[e->cur_inst];
      if (!mach_inst_preserves_r10_index(e, in))
        mach_r10_index_invalidate(e);
      int dst = mach_slot_off(e, &in->dst);
      int a = mach_slot_off(e, &in->src0);
      int b = mach_slot_off(e, &in->src1);
      if (!mach_begin_segments(e))
        return false;
      if (n == 0 && !mach_sync_carried_consumers(e, blk))
        return false;
      if (!mach_float_begin_segments(e))
        return false;
      if (!mach_vector_begin_segments(e))
        return false;
      /*
       * BR_IF reads an unfused condition from its canonical home. Materialize
       * it before the branch bytes. The former post-block scalar save
       * repeated this work after the terminator, adding dead or fall-through
       * traffic. Float and vector values have their own flush contract.
       */
      switch (in->opcode) {
      case NYR_MACH_COPY:
        if (mach_is_v128(mach, &in->dst)) {
          nyr_mach_type_t vt = mach_v128_type(mach, &in->dst);
          if (e->vec_fast_path && vt != NYR_MACH_TYPE_V256_I64 &&
              vt != NYR_MACH_TYPE_V256_F64 && mach_is_v128(mach, &in->src0)) {
            if (!mach_vector_copy(e, &in->dst, &in->src0, dst))
              return false;
            break;
          }
          if (e->vec_fast_path &&
              (vt == NYR_MACH_TYPE_V256_I64 || vt == NYR_MACH_TYPE_V256_F64)) {
            if (in->src0.kind == NYR_MACH_OPERAND_IMM) {
              /*
               * Broadcast an integer constant across the 256-bit value.
               * Materialize the 8-byte pattern in the destination home's
               * low lane, then broadcast from there — writing only the low
               * half would leave the upper lanes holding stale stack bytes.
               */
              if (!mach_mov_rax_imm(e, in->src0.as.imm) ||
                  !mach_store_rax(e, dst))
                return false;
              if (vt == NYR_MACH_TYPE_V256_F64) {
                if (!mach_ymm_broadcastsd_mem(e, 0, dst) ||
                    !mach_ymm_store_v256(e, dst, 0))
                  return false;
              } else {
                /*
                 * vpbroadcastq ymm0, [rbp+dst] — VEX.256.66.0F38.W1 59 /r
                 */
                if (!mach_u8(e, 0xc4) || !mach_u8(e, 0xe2) ||
                    !mach_u8(e, 0xfd) || !mach_u8(e, 0x59) ||
                    !mach_u8(e, 0x85) || !mach_i32(e, dst) ||
                    !mach_ymm_store_v256(e, dst, 0))
                  return false;
              }
              break;
            }
            if (!mach_is_v128(mach, &in->src0)) {
              /*
               * SET1 shape (VEC4_SET1 lowering): the source is one scalar
               * lane in an 8-byte home. Broadcast that lane instead of
               * copying 32 bytes — a plain ymm load would pull three lanes
               * of adjacent stack bytes into the value. A float source may
               * currently reside in a scalar float color without a home
               * write, so canonicalize it to its home first.
               */
              if (in->src0.kind == NYR_MACH_OPERAND_VREG) {
                int fp = mach_vreg_fpreg(e, in->src0.as.reg);
                if (fp >= 0 && e->fp_color_seeded &&
                    e->fp_color_seeded[in->src0.as.reg]) {
                  if (!mach_store_xmm(e, mach_vreg_home_off(e, in->src0.as.reg),
                                      e->mach->vreg_types[in->src0.as.reg] ==
                                          NYR_MACH_TYPE_F32,
                                      (unsigned)fp))
                    return false;
                  e->fp_color_seeded[in->src0.as.reg] = false;
                }
              }
              if (vt == NYR_MACH_TYPE_V256_F64) {
                if (!mach_ymm_broadcastsd_mem(e, 0, a) ||
                    !mach_ymm_store_v256(e, dst, 0))
                  return false;
              } else {
                /*
                 * vpbroadcastq ymm0, [rbp+a] — VEX.256.66.0F38.W1 59 /r
                 */
                if (!mach_u8(e, 0xc4) || !mach_u8(e, 0xe2) ||
                    !mach_u8(e, 0xfd) || !mach_u8(e, 0x59) ||
                    !mach_u8(e, 0x85) || !mach_i32(e, a) ||
                    !mach_ymm_store_v256(e, dst, 0))
                  return false;
              }
              break;
            }
            if (!mach_vector_ymm_copy(e, &in->dst, &in->src0, dst))
              return false;
            break;
          }
          if (vt == NYR_MACH_TYPE_V256_F64) {
            if (in->src0.kind == NYR_MACH_OPERAND_IMM) {
              if (!mach_mov_rax_imm(e, in->src0.as.imm) ||
                  !mach_store_rax(e, dst) ||
                  !mach_ymm_broadcastsd_mem(e, 0, dst) ||
                  !mach_ymm_store_v256(e, dst, 0))
                return false;
              break;
            }
            if (mach_is_float(mach, &in->src0)) {
              /*
               * SET1_F64: vbroadcastsd ymm0, [rbp+a]
               */
              if (!mach_ymm_broadcastsd_mem(e, 0, a) ||
                  !mach_ymm_store_v256(e, dst, 0))
                return false;
            } else if (mach_is_v128(mach, &in->src0)) {
              /*
               * Four-lane value copy: a ymm move, not a lane-truncating
               * xmm one.
               */
              if (!mach_ymm_load_v256(e, a, 0) ||
                  !mach_ymm_store_v256(e, dst, 0))
                return false;
            } else
              return mach_err(e, "x64 machine form encode: bad v256 f64 copy");
            break;
          }
          if (in->src0.kind == NYR_MACH_OPERAND_IMM) {
            /*
             * Broadcast imm to both qwords via two stores.
             */
            if (!mach_mov_rax_imm(e, in->src0.as.imm) ||
                !mach_store_rax(e, dst) || !mach_store_rax(e, dst - 8))
              return false;
          } else if (mach_is_v128(mach, &in->src0)) {
            if (!mach_load_xmm0_v128(e, a) || !mach_store_xmm0_v128(e, dst))
              return false;
          } else if (vt == NYR_MACH_TYPE_V128_I64) {
            /*
             * SET1_I64: movq scalar→xmm0; punpcklqdq self (broadcast).
             */
            if (!mach_load_op_rax(e, &in->src0) || !mach_u8(e, 0x66) ||
                !mach_rex(e, true, false, false, false) || !mach_u8(e, 0x0f) ||
                !mach_u8(e, 0x6e) || !mach_u8(e, 0xc0) || !mach_u8(e, 0x66) ||
                !mach_u8(e, 0x0f) || !mach_u8(e, 0x6c) || !mach_u8(e, 0xc0) ||
                !mach_store_xmm0_v128(e, dst))
              return false;
          } else if (vt == NYR_MACH_TYPE_V256_I64) {
            /*
             * SET1_I64 ymm: vpbroadcastq ymm0, [rbp+a]
             * VEX.256.66.0F38.W1 59 /r → C4 E3 FD 59 85 disp32
             */
            if (!mach_u8(e, 0xc4) || !mach_u8(e, 0xe2) || !mach_u8(e, 0xfd) ||
                !mach_u8(e, 0x59) || !mach_u8(e, 0x85) || !mach_i32(e, a) ||
                !mach_u8(e, 0xc4) || !mach_u8(e, 0xe1) || !mach_u8(e, 0x7d) ||
                !mach_u8(e, 0x7f) || !mach_u8(e, 0x85) || !mach_i32(e, dst))
              return false;
          } else if (vt == NYR_MACH_TYPE_V128_F64) {
            /*
             * SET1_F64: movddup.
             */
            if (!mach_load_xmm0(e, a, false) || !mach_u8(e, 0xf2) ||
                !mach_u8(e, 0x0f) || !mach_u8(e, 0x12) || !mach_u8(e, 0xc0) ||
                !mach_store_xmm0_v128(e, dst))
              return false;
          } else if (vt == NYR_MACH_TYPE_V128_F32) {
            /*
             * SET1_F32: movss + shufps $0.
             */
            if (!mach_load_xmm0(e, a, true) || !mach_u8(e, 0x0f) ||
                !mach_u8(e, 0xc6) || !mach_u8(e, 0xc0) || !mach_u8(e, 0x00) ||
                !mach_store_xmm0_v128(e, dst))
              return false;
          } else
            return mach_err(e, "x64 machine form encode: bad v128 copy");
          break;
        }
        if (mach_is_float(mach, &in->dst)) {
          bool f32 = mach_is_f32(mach, &in->dst);
          bool bitwise_gpr_source = in->src0.kind != NYR_MACH_OPERAND_IMM &&
                                    !mach_is_float(mach, &in->src0);
          if (e->fp_fast_path && !bitwise_gpr_source) {
            if (!mach_float_copy(e, &in->dst, &in->src0, dst, f32))
              return false;
          } else if (in->src0.kind == NYR_MACH_OPERAND_IMM) {
            if (!mach_mov_rax_imm(e, in->src0.as.imm) || !mach_u8(e, 0x66) ||
                !mach_rex(e, true, false, false, false) || !mach_u8(e, 0x0f) ||
                !mach_u8(e, 0x6e) || !mach_u8(e, 0xc0) ||
                !mach_store_xmm0(e, dst, f32))
              return false;
          } else if (!mach_load_xmm0(e, a, f32) ||
                     !mach_store_xmm0(e, dst, f32))
            return false;
          break;
        }
        if (!mach_is_i64(mach, &in->dst) &&
            in->dst.kind != NYR_MACH_OPERAND_NONE)
          return mach_err(e, "x64 machine form encode: unsupported COPY type");
        /*
         * A constant whose complete use set is encoded as RHS immediates has
         * no runtime value to materialize.  In particular, an uncolored
         * literal would otherwise unconditionally write an unread home in
         * mach_commit_vreg's spill fallback.
         */
        if (in->dst.kind == NYR_MACH_OPERAND_VREG &&
            in->dst.as.reg < e->mach->vreg_len && e->vreg_literal_rhs_only &&
            e->vreg_literal_rhs_only[in->dst.as.reg])
          break;
        /*
         * Loop-carried phi copy: the source already resides in the physical
         * register assigned to the destination, so the copy is a no-op.
         */
        if (in->src0.kind == NYR_MACH_OPERAND_VREG &&
            in->dst.kind == NYR_MACH_OPERAND_VREG && e->color_seeded &&
            in->src0.as.reg < e->colors_len && in->dst.as.reg < e->colors_len) {
          int sp = mach_vreg_preg(e, in->src0.as.reg);
          int dp = mach_vreg_preg(e, in->dst.as.reg);
          /*
           * Only elide a same-preg copy when the carry pass explicitly
           * validated the chain (dst segment marked carried).  Without the
           * carried flag the elision would skip the home commit that a
           * later block's live-in reload depends on, leaving the canonical
           * slot stale and the loop reading garbage.
           */
          const nyr_mach_live_segment_t *dseg =
              mach_segment_at(e, in->dst.as.reg, e->cur_inst);
          if (sp >= 0 && sp == dp && dseg && e->color_seeded[in->src0.as.reg] &&
              (dseg->carried ||
               (!mach_vreg_future_use_requires_home(e, in->dst.as.reg) &&
                !mach_vreg_used_in_later_block(e, in->dst.as.reg)))) {
            /*
             * Post-allocation copy coalescing: the destination already lives
             * in the required register.  Keep the explicit copy only when a
             * same-segment consumer requires the destination's stack home.
             */
            e->color_seeded[in->dst.as.reg] = true;
            break;
          }
        }
        /*
         * Different-preg copy: move directly between physical registers
         * instead of staging through %rax. Same-preg copies were handled
         * above; an unseeded source or an uncolored destination falls back
         * to the load/commit path below.
         */
        if (in->src0.kind == NYR_MACH_OPERAND_VREG &&
            in->dst.kind == NYR_MACH_OPERAND_VREG &&
            mach_is_i64(mach, &in->dst) && e->color_seeded &&
            in->src0.as.reg < e->colors_len && in->dst.as.reg < e->colors_len &&
            !in->narrow32) {
          bool saved_rax_valid = e->last_rax_valid;
          int saved_rax_off = e->last_rax_off;
          if (!mach_seed_colored_preg(e, in->src0.as.reg))
            return false;
          int sp = mach_vreg_preg(e, in->src0.as.reg);
          int dp = mach_vreg_def_preg(e, in->dst.as.reg);
          if (sp >= 0 && dp >= 0 && sp != dp) {
            if (!mach_mov_preg_preg(e, dp, sp))
              return false;
            /*
             * Same bookkeeping as mach_commit_vreg: clobber every other
             * seeded value sharing this preg, keep the canonical home
             * current when a later consumer re-seeds from it.
             */
            if (e->color_seeded) {
              for (uint32_t v = 0; v < e->colors_len; ++v) {
                if (v == in->dst.as.reg || !e->color_seeded[v])
                  continue;
                int other = mach_vreg_preg(e, v);
                if (other == dp)
                  e->color_seeded[v] = false;
              }
            }
            if (mach_vreg_requires_home_after_def(e, in->dst.as.reg)) {
              if (!mach_store_preg_home(e, dp,
                                        mach_vreg_home_off(e, in->dst.as.reg)))
                return false;
            }
            /* Seeding and copying between non-RAX registers preserve RAX.
             * Retain its cached home only if this definition cannot replace
             * the value stored at that same home. Segment-end stores still
             * invalidate the cache through their existing path. */
            if (saved_rax_valid && dp != 0 && sp != 0 &&
                saved_rax_off != mach_vreg_home_off(e, in->dst.as.reg)) {
              e->last_rax_valid = true;
              e->last_rax_off = saved_rax_off;
            } else {
              mach_rax_invalidate(e);
            }
            e->color_seeded[in->dst.as.reg] = true;
            break;
          }
        }
        if (!mach_load_op_rax(e, &in->src0))
          return false;
        if (!mach_commit_vreg(e, &in->dst, dst))
          return false;
        break;
      case NYR_MACH_LOAD: {
        if (mach_is_v128(mach, &in->dst)) {
          nyr_mach_type_t vt = mach_v128_type(mach, &in->dst);
          if (e->vec_fast_path && vt != NYR_MACH_TYPE_V256_I64 &&
              vt != NYR_MACH_TYPE_V256_F64) {
            if (!mach_vector_load(e, in, dst))
              return false;
            break;
          }
          if (e->vec_fast_path &&
              (vt == NYR_MACH_TYPE_V256_I64 || vt == NYR_MACH_TYPE_V256_F64)) {
            if (vt == NYR_MACH_TYPE_V256_F64 &&
                e->cur_inst + 1 < mach->inst_len &&
                mach_vector_ymm_rhs_load(e, e->cur_inst, e->cur_inst + 1,
                                         &mach->insts[e->cur_inst + 1].src1))
              break;
            if (!mach_vector_ymm_load(e, in, dst))
              return false;
            break;
          }
          if (vt == NYR_MACH_TYPE_V256_I64 || vt == NYR_MACH_TYPE_V256_F64) {
            /*
             * vmovdqa ymm0, [rbp+disp32] — VEX.256.66.0F 6F /r
             */
            if (in->src0.kind == NYR_MACH_OPERAND_FRAME) {
              if (!mach_u8(e, 0xc4) || !mach_u8(e, 0xe1) || !mach_u8(e, 0x7e) ||
                  !mach_u8(e, 0x6f) || !mach_u8(e, 0x85) || !mach_i32(e, a))
                return false;
            } else if (in->src0.kind == NYR_MACH_OPERAND_VREG) {
              if (!mach_load_op_rax(e, &in->src0) || !mach_u8(e, 0xc4) ||
                  !mach_u8(e, 0xe1) || !mach_u8(e, 0x7e) || !mach_u8(e, 0x6f) ||
                  !mach_u8(e, 0x00))
                return false;
            } else
              return mach_err(e, "x64 machine form encode: v256 load");
            /*
             * Store ymm0 back to home.
             */
            if (!mach_u8(e, 0xc4) || !mach_u8(e, 0xe1) || !mach_u8(e, 0x7e) ||
                !mach_u8(e, 0x7f) || !mach_u8(e, 0x85) || !mach_i32(e, dst))
              return false;
            break;
          }
          if (in->src0.kind == NYR_MACH_OPERAND_FRAME) {
            if (!mach_load_xmm0_v128(e, a) || !mach_store_xmm0_v128(e, dst))
              return false;
          } else if (in->src0.kind == NYR_MACH_OPERAND_VREG) {
            if (!mach_load_rax(e, a) || !mach_u8(e, 0xf3) ||
                !mach_u8(e, 0x0f) || !mach_u8(e, 0x6f) || !mach_u8(e, 0x00) ||
                !mach_store_xmm0_v128(e, dst))
              return false;
          } else
            return mach_err(e, "x64 machine form encode: v128 load");
          break;
        }
        if (mach_is_float(mach, &in->dst)) {
          bool f32 = mach_is_f32(mach, &in->dst);
          if (e->fp_fast_path && in->src0.kind == NYR_MACH_OPERAND_FRAME) {
            int dreg = in->dst.kind == NYR_MACH_OPERAND_VREG
                           ? mach_vreg_fpreg(e, in->dst.as.reg)
                           : -1;
            unsigned reg = dreg >= 0 ? (unsigned)dreg : 14;
            if (!mach_load_xmm(e, a, f32, reg))
              return false;
            if (dreg >= 0) {
              if (e->fp_color_seeded)
                e->fp_color_seeded[in->dst.as.reg] = true;
            } else if (!mach_store_xmm(e, dst, f32, reg))
              return false;
          } else if (in->src0.kind == NYR_MACH_OPERAND_FRAME) {
            if (!mach_load_xmm0(e, a, f32) || !mach_store_xmm0(e, dst, f32))
              return false;
          } else if (in->src0.kind == NYR_MACH_OPERAND_VREG) {
            /*
             * mov ptr to rax; movs[sd] (rax), xmm0; store
             */
            int dreg = e->fp_fast_path && in->dst.kind == NYR_MACH_OPERAND_VREG
                           ? mach_vreg_fpreg(e, in->dst.as.reg)
                           : -1;
            unsigned reg = dreg >= 0 ? (unsigned)dreg : 14;
            ny_x64_mem_addr_t A;
            if (!mach_resolve_mem_addr(e, &in->src0, &A))
              return false;
            if (!mach_u8(e, f32 ? 0xf3 : 0xf2) ||
                !mach_rex_for_addr(e, &A, false, reg >= 8) ||
                !mach_u8(e, 0x0f) || !mach_u8(e, 0x10) ||
                !mach_emit_mem(e, &A, reg & 7))
              return false;
            if (dreg >= 0) {
              if (e->fp_color_seeded)
                e->fp_color_seeded[in->dst.as.reg] = true;
            } else if (!mach_store_xmm(e, dst, f32, reg))
              return false;
          } else
            return mach_err(e, "x64 machine form encode: float load from ptr");
          break;
        }
        if (in->src0.kind == NYR_MACH_OPERAND_FRAME) {
          int incoming = e->direct_param_source && e->cur_inst < mach->inst_len
                             ? e->direct_param_source[e->cur_inst]
                             : -1;
          int preg = in->dst.kind == NYR_MACH_OPERAND_VREG
                         ? mach_vreg_def_preg(e, in->dst.as.reg)
                         : -1;
          if (incoming >= 0 && preg >= 0) {
            if (ny_dbg_is_enabled(NY_DBG_DEEP)) {
              ny_dbg_enter_context(e->debug_name);
              ny_dbg_heuristic_event("X64_PARAM_DIRECT",
                                     "load inst=%zu src=%d dst=%d", e->cur_inst,
                                     incoming, preg);
              ny_dbg_exit_context(e->debug_name);
            }
            if (!mach_mov_preg_preg(e, preg, incoming) ||
                !mach_commit_preg(e, &in->dst, dst, (unsigned)preg, false))
              return false;
          } else if (preg >= 0) {
            if (!mach_load_home_preg(e, a, (unsigned)preg) ||
                !mach_commit_preg(e, &in->dst, dst, (unsigned)preg, a == dst))
              return false;
          } else if (!mach_load_rax(e, a) ||
                     !mach_commit_vreg(e, &in->dst, dst)) {
            return false;
          }
        } else {
          ny_x64_mem_addr_t A;
          if (!mach_resolve_mem_addr(e, &in->src0, &A))
            return false;
          if (in->byte_width) {
            if (!mach_rex_for_addr(e, &A, false, false) || !mach_u8(e, 0x0f) ||
                !mach_u8(e, 0xb6) || !mach_emit_mem(e, &A, 0))
              return false;
          } else if (!mach_rex_for_addr(e, &A, true, false) ||
                     !mach_u8(e, 0x8b) || !mach_emit_mem(e, &A, 0)) {
            return false;
          }
          if (!mach_commit_vreg(e, &in->dst, dst))
            return false;
        }
        break;
      }
      case NYR_MACH_STORE:
        if (mach_is_v128(mach, &in->src0)) {
          nyr_mach_type_t vt = mach_v128_type(mach, &in->src0);
          if (e->vec_fast_path && vt != NYR_MACH_TYPE_V256_I64 &&
              vt != NYR_MACH_TYPE_V256_F64) {
            if (!mach_vector_store(e, in, dst))
              return false;
            break;
          }
          if (vt == NYR_MACH_TYPE_V256_I64 || vt == NYR_MACH_TYPE_V256_F64) {
            if (!mach_vector_ymm_store(e, in, dst))
              return false;
            break;
          }
          if (vt == NYR_MACH_TYPE_V256_I64 || vt == NYR_MACH_TYPE_V256_F64) {
            /*
             * Load src0 to ymm0, then vmovdqa [dest], ymm0.
             */
            if (in->src0.kind == NYR_MACH_OPERAND_FRAME) {
              if (!mach_u8(e, 0xc4) || !mach_u8(e, 0xe1) || !mach_u8(e, 0x7e) ||
                  !mach_u8(e, 0x6f) || !mach_u8(e, 0x85) || !mach_i32(e, a))
                return false;
            } else if (in->src0.kind == NYR_MACH_OPERAND_VREG) {
              /*
               * src0 is the stored value, not an address: load it from its
               * own 32-byte home into ymm0.
               */
              if (!mach_ymm_load_v256(e, a, 0))
                return false;
            } else
              return mach_err(e, "x64 machine form encode: v256 store src");
            if (in->dst.kind == NYR_MACH_OPERAND_FRAME) {
              if (!mach_u8(e, 0xc4) || !mach_u8(e, 0xe1) || !mach_u8(e, 0x7e) ||
                  !mach_u8(e, 0x7f) || !mach_u8(e, 0x85) || !mach_i32(e, dst))
                return false;
            } else if (in->dst.kind == NYR_MACH_OPERAND_VREG) {
              if (!mach_load_op_rax(e, &in->dst) || !mach_u8(e, 0xc4) ||
                  !mach_u8(e, 0xe1) || !mach_u8(e, 0x7e) || !mach_u8(e, 0x7f) ||
                  !mach_u8(e, 0x00))
                return false;
            } else
              return mach_err(e, "x64 machine form encode: v256 store");
            break;
          }
          if (in->dst.kind == NYR_MACH_OPERAND_FRAME) {
            if (!mach_load_xmm0_v128(e, a) || !mach_store_xmm0_v128(e, dst))
              return false;
          } else if (in->dst.kind == NYR_MACH_OPERAND_VREG) {
            /*
             * store xmm0 → *ptr : mov ptr→rax; movdqu [rax], xmm0
             */
            if (!mach_load_op_rax(e, &in->dst) || !mach_load_xmm0_v128(e, a) ||
                !mach_u8(e, 0xf3) || !mach_u8(e, 0x0f) || !mach_u8(e, 0x7f) ||
                !mach_u8(e, 0x00))
              return false;
          } else
            return mach_err(e, "x64 machine form encode: v128 store");
          break;
        }
        if (mach_is_float(mach, &in->src0)) {
          bool f32 = mach_is_f32(mach, &in->src0);
          if (in->dst.kind == NYR_MACH_OPERAND_FRAME) {
            if (e->fp_fast_path) {
              unsigned reg = 14;
              if (!mach_load_float_operand(e, &in->src0, f32, 14, &reg) ||
                  !mach_store_xmm(e, dst, f32, reg))
                return false;
            } else if (!mach_load_xmm0(e, a, f32) ||
                       !mach_store_xmm0(e, dst, f32))
              return false;
          } else if (in->dst.kind == NYR_MACH_OPERAND_VREG) {
            unsigned reg = 14;
            ny_x64_mem_addr_t A;
            if (!mach_resolve_mem_addr(e, &in->dst, &A))
              return false;
            if (e->fp_fast_path) {
              if (!mach_load_float_operand(e, &in->src0, f32, 14, &reg) ||
                  !mach_u8(e, f32 ? 0xf3 : 0xf2) ||
                  !mach_rex_for_addr(e, &A, false, reg >= 8) ||
                  !mach_u8(e, 0x0f) || !mach_u8(e, 0x11) ||
                  !mach_emit_mem(e, &A, reg & 7))
                return false;
            } else if (!mach_load_xmm0(e, a, f32) ||
                       !mach_u8(e, f32 ? 0xf3 : 0xf2) ||
                       !mach_rex_for_addr(e, &A, false, false) ||
                       !mach_u8(e, 0x0f) || !mach_u8(e, 0x11) ||
                       !mach_emit_mem(e, &A, 0))
              return false;
          }
          break;
        }
        if (in->dst.kind == NYR_MACH_OPERAND_FRAME) {
          if (!mach_load_op_rax(e, &in->src0) || !mach_store_rax(e, dst))
            return false;
        } else {
          int64_t simm = 0;
          bool is_imm = false;
          if (in->src0.kind == NYR_MACH_OPERAND_IMM) {
            simm = in->src0.as.imm;
            is_imm = true;
          } else if (in->src0.kind == NYR_MACH_OPERAND_VREG && imm_fold &&
                     imm_fold_known && in->src0.as.reg < mach->vreg_len &&
                     imm_fold_known[in->src0.as.reg]) {
            simm = imm_fold[in->src0.as.reg];
            is_imm = true;
          }
          if (is_imm && (int64_t)(int32_t)simm == simm) {
            ny_x64_mem_addr_t A;
            if (!mach_resolve_mem_addr(e, &in->dst, &A))
              return false;
            if (in->byte_width) {
              if (!mach_rex_for_addr(e, &A, false, false) ||
                  !mach_u8(e, 0xc6) || !mach_emit_mem(e, &A, 0) ||
                  !mach_u8(e, (unsigned char)simm))
                return false;
            } else {
              if (!mach_rex_for_addr(e, &A, true, false) || !mach_u8(e, 0xc7) ||
                  !mach_emit_mem(e, &A, 0) || !mach_i32(e, (int32_t)simm))
                return false;
            }
          } else {
            ny_x64_mem_addr_t A;
            unsigned src_preg = 0;
            if (!mach_resolve_mem_addr(e, &in->dst, &A) ||
                !mach_store_value_preg(e, &A, &in->src0, &src_preg))
              return false;
            if (in->byte_width) {
              if (!mach_rex_for_addr(e, &A, false, src_preg >= 8) ||
                  !mach_u8(e, 0x88) || !mach_emit_mem(e, &A, src_preg & 7))
                return false;
            } else if (!mach_rex_for_addr(e, &A, true, src_preg >= 8) ||
                       !mach_u8(e, 0x89) ||
                       !mach_emit_mem(e, &A, src_preg & 7)) {
              return false;
            }
          }
        }
        break;
      case NYR_MACH_LEA: {
        /*
         * The vector load encoder consumes a pointer value in a GPR; it does
         * not fold an indexed LEA into the memory operand. Keep this address
         * definition materialized even when the following vector load is its
         * only use, or the load reads an uninitialized vreg home.
         */
        bool next_is_vector_load =
            e->cur_inst + 1 < mach->inst_len &&
            mach->insts[e->cur_inst + 1].opcode == NYR_MACH_LOAD &&
            mach_is_v128(mach, &mach->insts[e->cur_inst + 1].dst);
        if (in->dst.kind == NYR_MACH_OPERAND_VREG && !next_is_vector_load &&
            ((mach_sib_lea_mem(e, in->dst.as.reg, e->cur_inst + 1, NULL, NULL,
                               NULL, NULL)) ||
             (in->src0.kind == NYR_MACH_OPERAND_VREG &&
              in->src1.kind == NYR_MACH_OPERAND_VREG &&
              in->src2.kind == NYR_MACH_OPERAND_IMM &&
              (in->src2.as.imm == 2 || in->src2.as.imm == 4 ||
               in->src2.as.imm == 8) &&
              mach_sib_lea_all_uses_mem(e, in->dst.as.reg, e->cur_inst))))
          break;
        if (in->src0.kind == NYR_MACH_OPERAND_VREG &&
            in->src1.kind == NYR_MACH_OPERAND_VREG &&
            in->src2.kind == NYR_MACH_OPERAND_IMM &&
            (in->src2.as.imm == 2 || in->src2.as.imm == 4 ||
             in->src2.as.imm == 8)) {
          unsigned scale = in->src2.as.imm == 2   ? 1
                           : in->src2.as.imm == 4 ? 2
                                                  : 3;
          int base_preg = mach_vreg_preg(e, in->src0.as.reg);
          int index_preg = mach_vreg_preg(e, in->src1.as.reg);
          int assigned_base = mach_vreg_assigned_preg(e, in->src0.as.reg);
          int assigned_index = mach_vreg_assigned_preg(e, in->src1.as.reg);
          if (base_preg < 0 && assigned_base >= 0 &&
              !mach_seed_colored_preg(e, in->src0.as.reg))
            return false;
          if (index_preg < 0 && assigned_index >= 0 &&
              !mach_seed_colored_preg(e, in->src1.as.reg))
            return false;
          base_preg = mach_vreg_preg(e, in->src0.as.reg);
          index_preg = mach_vreg_preg(e, in->src1.as.reg);
          if (base_preg >= 0 || (index_preg >= 0 && index_preg != 4)) {
            /*
             * Use any seeded input registers directly in the LEA. The old
             * scratch-only sequence moved every base through RAX and every
             * index through R10, even when allocation had already kept them
             * resident. Spill only the missing input; RSP cannot be an SIB
             * index, but R12 remains valid with REX.X set.
             */
            unsigned index = 0;
            if (index_preg >= 0 && index_preg != 4) {
              index = (unsigned)index_preg;
            } else if (!mach_load_op_rax(e, &in->src1) ||
                       !mach_mov_reg_rax(e, 10)) {
              return false;
            } else {
              index = 10;
            }
            unsigned base = 0;
            if (base_preg >= 0) {
              base = (unsigned)base_preg;
            } else if (!mach_load_op_rax(e, &in->src0)) {
              return false;
            }
            int dst_preg = mach_vreg_def_preg(e, in->dst.as.reg);
            unsigned dst_reg = dst_preg >= 0 ? (unsigned)dst_preg : 0;
            bool needs_zero_disp = (base & 7) == 5;
            unsigned mod = needs_zero_disp ? 1u : 0u;
            if (!mach_rex(e, true, dst_reg >= 8, index >= 8, base >= 8) ||
                !mach_u8(e, 0x8d) ||
                !mach_u8(e, (mod << 6) | ((dst_reg & 7) << 3) | 0x04) ||
                !mach_u8(e, (scale << 6) | ((index & 7) << 3) | (base & 7)) ||
                (needs_zero_disp && !mach_u8(e, 0)))
              return false;
            if (dst_preg >= 0) {
              if (!mach_commit_preg(e, &in->dst, dst, dst_reg, false))
                return false;
            } else {
              mach_rax_invalidate(e);
              if (!mach_commit_vreg(e, &in->dst, dst))
                return false;
            }
            break;
          }
          /*
           * lea (base,index,scale),rax; r10 is reserved encoder scratch.
           */
          if (!mach_load_op_rax(e, &in->src1) || !mach_mov_reg_rax(e, 10) ||
              !mach_load_op_rax(e, &in->src0) ||
              !mach_rex(e, true, false, true, false) || !mach_u8(e, 0x8d) ||
              !mach_u8(e, 0x04) || !mach_u8(e, (scale << 6) | 0x10) ||
              !mach_commit_vreg(e, &in->dst, dst))
            return false;
        } else if (in->src0.kind == NYR_MACH_OPERAND_VREG &&
                   in->src1.kind == NYR_MACH_OPERAND_IMM &&
                   (in->src1.as.imm == 2 || in->src1.as.imm == 4 ||
                    in->src1.as.imm == 8)) {
          unsigned scale = in->src1.as.imm == 2   ? 1
                           : in->src1.as.imm == 4 ? 2
                                                  : 3;
          /*
           * lea (%rax,%rax,scale), %rax; RAX is the encoder scratch.
           */
          if (!mach_load_op_rax(e, &in->src0) ||
              !mach_rex(e, true, false, false, false) || !mach_u8(e, 0x8d) ||
              !mach_u8(e, 0x04) || !mach_u8(e, (scale << 6) | 0x00) ||
              !mach_commit_vreg(e, &in->dst, dst))
            return false;
        } else if (in->src0.kind == NYR_MACH_OPERAND_FRAME) {
          if (!mach_rex(e, true, false, false, false) || !mach_u8(e, 0x8d) ||
              !mach_u8(e, 0x85) || !mach_i32(e, a) ||
              !mach_commit_vreg(e, &in->dst, dst))
            return false;
        } else if (in->src0.kind == NYR_MACH_OPERAND_SYMBOL &&
                   in->src0.as.symbol) {
          /*
           * leaq sym(%rip), %rax — 48 8d 05 rel32
           */
          if (!mach_rex(e, true, false, false, false) || !mach_u8(e, 0x8d) ||
              !mach_u8(e, 0x05) ||
              !mach_add_reloc(e, in->src0.as.symbol, NY_RELOC_PC32) ||
              !mach_i32(e, 0) || !mach_commit_vreg(e, &in->dst, dst))
            return false;
        } else
          return mach_err(e, "x64 machine form encode: bad LEA");
        break;
      }
      case NYR_MACH_CONVERT:
        if (mach_is_f64(mach, &in->dst) && mach_is_i64(mach, &in->src0)) {
          /*
           * cvtsi2sdq %rax, %xmm0.  The i64 source may be a colored GPR
           * vreg whose home slot was never written (const COPY commits only
           * the preg), so load through mach_load_op_rax which seeds the
           * colored preg instead of reading a stale home.
           */
          int dreg = in->dst.kind == NYR_MACH_OPERAND_VREG
                         ? mach_vreg_fpreg(e, in->dst.as.reg)
                         : -1;
          unsigned d = dreg >= 0 ? (unsigned)dreg : 0;
          if (!mach_load_op_rax(e, &in->src0) || !mach_u8(e, 0xf2) ||
              !mach_rex(e, true, dreg >= 8, false, false) ||
              !mach_u8(e, 0x0f) || !mach_u8(e, 0x2a) ||
              /*
               * ModRM: mod=11, reg=DEST xmm (d), rm=RAX (the loaded src)
               */
              !mach_u8(e, (unsigned)(0xc0 | ((d & 7) << 3))) ||
              !mach_store_xmm(e, dst, false, d))
            return false;
          if (dreg >= 0) {
            if (e->fp_color_seeded)
              e->fp_color_seeded[in->dst.as.reg] = false;
          }
          break;
        } else if (mach_is_f32(mach, &in->dst) &&
                   mach_is_i64(mach, &in->src0)) {
          int dreg = in->dst.kind == NYR_MACH_OPERAND_VREG
                         ? mach_vreg_fpreg(e, in->dst.as.reg)
                         : -1;
          unsigned d = dreg >= 0 ? (unsigned)dreg : 0;
          if (!mach_load_op_rax(e, &in->src0) || !mach_u8(e, 0xf3) ||
              !mach_rex(e, true, dreg >= 8, false, false) ||
              !mach_u8(e, 0x0f) || !mach_u8(e, 0x2a) ||
              !mach_u8(e, (unsigned)(0xc0 | ((d & 7) << 3))) ||
              !mach_store_xmm(e, dst, true, d))
            return false;
          if (dreg >= 0) {
            if (e->fp_color_seeded)
              e->fp_color_seeded[in->dst.as.reg] = false;
          }
          break;
        } else if (mach_is_f64(mach, &in->dst) &&
                   mach_is_f32(mach, &in->src0)) {
          /*
           * cvtss2sd: the source may be a fast-path-colored float vreg whose
           * home slot was never written (mach_float_copy skips the home store
           * for colored vregs), so load through mach_load_float_operand and
           * keep the result in the destination color when assigned.
           */
          unsigned d = 14, src_reg = 14;
          int assigned = in->dst.kind == NYR_MACH_OPERAND_VREG
                             ? mach_vreg_fpreg(e, in->dst.as.reg)
                             : -1;
          if (assigned >= 0)
            d = (unsigned)assigned;
          if (!mach_load_float_operand(e, &in->src0, true, d, &src_reg))
            return false;
          if (src_reg != d && !mach_sse_mov_reg(e, d, src_reg, true))
            return false;
          if (!mach_sse_op_reg(e, d, d, true, 0x5a))
            return false;
          if (assigned >= 0) {
            if (e->fp_color_seeded)
              e->fp_color_seeded[in->dst.as.reg] = true;
          } else if (!mach_store_xmm(e, dst, false, d))
            return false;
        } else if (mach_is_f32(mach, &in->dst) &&
                   mach_is_f64(mach, &in->src0)) {
          /*
           * cvtsd2ss: same colored-source handling as cvtss2sd above.
           */
          unsigned d = 14, src_reg = 14;
          int assigned = in->dst.kind == NYR_MACH_OPERAND_VREG
                             ? mach_vreg_fpreg(e, in->dst.as.reg)
                             : -1;
          if (assigned >= 0)
            d = (unsigned)assigned;
          if (!mach_load_float_operand(e, &in->src0, false, d, &src_reg))
            return false;
          if (src_reg != d && !mach_sse_mov_reg(e, d, src_reg, false))
            return false;
          if (!mach_sse_op_reg(e, d, d, false, 0x5a))
            return false;
          if (assigned >= 0) {
            if (e->fp_color_seeded)
              e->fp_color_seeded[in->dst.as.reg] = true;
          } else if (!mach_store_xmm(e, dst, true, d))
            return false;
        } else if (mach_is_i64(mach, &in->dst) &&
                   (mach_is_f64(mach, &in->src0) ||
                    mach_is_f32(mach, &in->src0))) {
          bool f32 = mach_is_f32(mach, &in->src0);
          unsigned src_reg = 15;
          if (!mach_load_float_operand(e, &in->src0, f32, 15, &src_reg))
            return false;
          if (src_reg != 0 && !mach_sse_mov_reg(e, 0, src_reg, f32))
            return false;
          if (!mach_u8(e, f32 ? 0xf3 : 0xf2) ||
              !mach_rex(e, true, false, false, false) || !mach_u8(e, 0x0f) ||
              !mach_u8(e, 0x2c) || !mach_u8(e, 0xc0) ||
              !mach_commit_vreg(e, &in->dst, dst))
            return false;
        } else
          return mach_err(e, "x64 machine form encode: unsupported CONVERT");
        break;
      case NYR_MACH_SQRT: {
        bool is_f32 = false;
        if (mach_is_f64(mach, &in->dst) && mach_is_f64(mach, &in->src0)) {
          is_f32 = false;
        } else if (mach_is_f32(mach, &in->dst) &&
                   mach_is_f32(mach, &in->src0)) {
          is_f32 = true;
        } else
          return mach_err(e, "x64 machine form encode: unsupported SQRT type");

        unsigned d = 14, src_reg = 14;
        int assigned = in->dst.kind == NYR_MACH_OPERAND_VREG
                           ? mach_vreg_fpreg(e, in->dst.as.reg)
                           : -1;
        if (assigned >= 0)
          d = (unsigned)assigned;

        if (!mach_load_float_operand(e, &in->src0, is_f32, d, &src_reg))
          return false;
        if (src_reg != d && !mach_sse_mov_reg(e, d, src_reg, is_f32))
          return false;

        if (!mach_sse_op_reg(e, d, d, is_f32, 0x51))
          return false;

        if (assigned >= 0) {
          if (e->fp_color_seeded)
            e->fp_color_seeded[in->dst.as.reg] = true;
        } else if (!mach_store_xmm(e, dst, is_f32, d))
          return false;
        break;
      }
      case NYR_MACH_SIN:
      case NYR_MACH_COS:
        if (!mach_is_f64(mach, &in->dst) || !mach_is_f64(mach, &in->src0))
          return mach_err(e, "x64 machine form encode: unsupported trig type");
        if (!mach_u8(e, 0xdd) || !mach_u8(e, 0x85) || !mach_i32(e, a) ||
            !mach_u8(e, 0xd9) ||
            !mach_u8(e, in->opcode == NYR_MACH_SIN ? 0xfe : 0xff) ||
            !mach_u8(e, 0xdd) || !mach_u8(e, 0x9d) || !mach_i32(e, dst))
          return false;
        break;
      case NYR_MACH_BSWAP64:
      case NYR_MACH_BSWAP32:
        if (!mach_is_i64(mach, &in->dst) || !mach_is_i64(mach, &in->src0))
          return mach_err(e,
                          "x64 machine form encode: bswap needs i64 operands");
        if (!mach_load_op_rax(e, &in->src0))
          return false;
        if (in->opcode == NYR_MACH_BSWAP64) {
          if (!mach_rex(e, true, false, false, false))
            return false;
        }
        /*
         * bswap rax/eax; the 32-bit form zero-extends into rax.
         */
        if (!mach_u8(e, 0x0f) || !mach_u8(e, 0xc8) || !mach_store_rax(e, dst))
          return false;
        break;
      case NYR_MACH_ADD:
      case NYR_MACH_SUB:
      case NYR_MACH_AND:
      case NYR_MACH_OR:
      case NYR_MACH_XOR:
      case NYR_MACH_MUL:
      case NYR_MACH_DIV:
      case NYR_MACH_ROR:
      case NYR_MACH_ROR32:
      case NYR_MACH_SHL:
      case NYR_MACH_SAR:
        if (in->opcode == NYR_MACH_SHL &&
            in->dst.kind == NYR_MACH_OPERAND_VREG &&
            mach_sib_scaled_index_only_feeds_add(e, in->dst.as.reg))
          break;
        if (in->opcode == NYR_MACH_AND &&
            in->dst.kind == NYR_MACH_OPERAND_VREG && e->select_skip_and &&
            in->dst.as.reg < e->colors_len &&
            e->select_skip_and[in->dst.as.reg])
          break;
        if (in->opcode == NYR_MACH_ADD &&
            in->dst.kind == NYR_MACH_OPERAND_VREG &&
            e->cur_inst + 1 < mach->inst_len &&
            (mach->insts[e->cur_inst + 1].opcode == NYR_MACH_LOAD ||
             (mach->insts[e->cur_inst + 1].opcode == NYR_MACH_STORE &&
              mach_op_is_vreg(&mach->insts[e->cur_inst + 1].dst,
                              in->dst.as.reg)))) {
          const nyr_mach_operand_t *base = NULL, *index = NULL;
          const nyr_mach_operand_t *scaled_value = NULL;
          unsigned scale = 0;
          if (mach_sib_add_mem(e, in->dst.as.reg, e->cur_inst + 1, &base,
                               &index, &scale, &scaled_value)) {
            int bp = base->kind == NYR_MACH_OPERAND_VREG
                         ? mach_vreg_preg(e, base->as.reg)
                         : -1;
            int ip = index->kind == NYR_MACH_OPERAND_VREG
                         ? mach_vreg_preg(e, index->as.reg)
                         : -1;
            if (bp >= 0 && ip >= 0 && ip != 4 &&
                mach_seed_colored_preg(e, base->as.reg) &&
                mach_seed_colored_preg(e, index->as.reg)) {
              /* Consume loop-resident address operands directly in SIB. */
              e->pending_sib_base = (unsigned)bp;
              e->pending_sib_index = (unsigned)ip;
            } else {
              /* Otherwise materialize the address pair in reserved scratches.
               */
              if (!mach_load_op_preg(e, index, 10) ||
                  !mach_load_op_rax(e, base))
                return false;
              e->pending_sib_base = 0;
              e->pending_sib_index = 10;
            }
            e->pending_sib_vreg = in->dst.as.reg;
            e->pending_sib_scale = scale;
            e->pending_sib_valid = true;
            break;
          }
        }
        if (mach_is_v128(mach, &in->dst)) {
          nyr_mach_type_t vt = mach_v128_type(mach, &in->dst);
          if (e->vec_fast_path && vt == NYR_MACH_TYPE_V256_F64 &&
              (in->opcode == NYR_MACH_ADD || in->opcode == NYR_MACH_SUB ||
               in->opcode == NYR_MACH_MUL || in->opcode == NYR_MACH_DIV)) {
            unsigned char opc = in->opcode == NYR_MACH_ADD   ? 0x58
                                : in->opcode == NYR_MACH_SUB ? 0x5c
                                : in->opcode == NYR_MACH_MUL ? 0x59
                                                             : 0x5e;
            if (!mach_vector_ymm_fp_binop(e, in, dst, opc))
              return false;
            break;
          }
          if (e->vec_fast_path &&
              (vt == NYR_MACH_TYPE_V128_F64 || vt == NYR_MACH_TYPE_V128_F32)) {
            unsigned prefix =
                vt == NYR_MACH_TYPE_V128_I64 || vt == NYR_MACH_TYPE_V128_F64
                    ? 0x66
                    : 0;
            unsigned opc =
                in->opcode == NYR_MACH_ADD   ? 0x58
                : in->opcode == NYR_MACH_SUB ? 0x5c
                : in->opcode == NYR_MACH_MUL ? 0x59
                : in->opcode == NYR_MACH_DIV ? 0x5e
                : vt == NYR_MACH_TYPE_V128_I64 && in->opcode == NYR_MACH_AND
                    ? 0xdb
                : vt == NYR_MACH_TYPE_V128_I64 && in->opcode == NYR_MACH_OR
                    ? 0xeb
                : vt == NYR_MACH_TYPE_V128_I64 && in->opcode == NYR_MACH_XOR
                    ? 0xef
                    : 0xff;
            if (opc == 0xff ||
                (vt != NYR_MACH_TYPE_V128_I64 &&
                 (in->opcode == NYR_MACH_AND || in->opcode == NYR_MACH_OR ||
                  in->opcode == NYR_MACH_XOR)))
              return mach_err(
                  e, "x64 machine form encode: unsupported vector ALU");
            if (!mach_vector_binop(e, in, dst, prefix, opc))
              return false;
            break;
          }
          if (e->vec_fast_path && vt == NYR_MACH_TYPE_V256_I64 &&
              in->opcode != NYR_MACH_MUL && in->opcode != NYR_MACH_DIV &&
              in->opcode != NYR_MACH_SHL && in->opcode != NYR_MACH_SAR) {
            unsigned char opc = in->opcode == NYR_MACH_ADD   ? 0xd4
                                : in->opcode == NYR_MACH_SUB ? 0xfb
                                : in->opcode == NYR_MACH_AND ? 0xdb
                                : in->opcode == NYR_MACH_OR  ? 0xeb
                                                             : 0xef;
            if (!mach_vector_ymm_binop(e, in, dst, opc))
              return false;
            break;
          }
          if (!mach_load_xmm0_v128(e, a))
            return false;
          if (vt == NYR_MACH_TYPE_V128_I64) {
            /*
             * paddq/psubq/pand/por/pxor — no packed i64 mul/div.
             */
            if (in->opcode == NYR_MACH_MUL || in->opcode == NYR_MACH_DIV)
              return mach_err(e,
                              "x64 machine form encode: no packed i64 mul/div");
            unsigned char opc = in->opcode == NYR_MACH_ADD   ? 0xd4
                                : in->opcode == NYR_MACH_SUB ? 0xfb
                                : in->opcode == NYR_MACH_AND ? 0xdb
                                : in->opcode == NYR_MACH_OR  ? 0xeb
                                                             : 0xef;
            if (!mach_vec_op_mem_reg(e, 0, b, 0x66, opc) ||
                !mach_store_xmm0_v128(e, dst))
              return false;
          } else if (vt == NYR_MACH_TYPE_V256_I64) {
            /*
             * vpaddq/vpsubq/vpand/vpor/vpxor ymm — VEX.256.66.0F.W1 opc
             */
            if (in->opcode == NYR_MACH_MUL || in->opcode == NYR_MACH_DIV)
              return mach_err(e,
                              "x64 machine form encode: no packed i64 mul/div");
            unsigned char opc = in->opcode == NYR_MACH_ADD   ? 0xd4
                                : in->opcode == NYR_MACH_SUB ? 0xfb
                                : in->opcode == NYR_MACH_AND ? 0xdb
                                : in->opcode == NYR_MACH_OR  ? 0xeb
                                                             : 0xef;
            if (!mach_u8(e, 0xc4) || !mach_u8(e, 0xe1) || !mach_u8(e, 0x7d) ||
                !mach_u8(e, opc) || !mach_u8(e, 0x85) || !mach_i32(e, b) ||
                !mach_u8(e, 0xc4) || !mach_u8(e, 0xe1) || !mach_u8(e, 0x7d) ||
                !mach_u8(e, 0x7f) || !mach_u8(e, 0x85) || !mach_i32(e, dst))
              return false;
          } else if (vt == NYR_MACH_TYPE_V256_F64) {
            /*
             * vaddpd/vsubpd/vmulpd/vdivpd ymm0, ymm0, [rbp+b] —
             * VEX.256.66.0F.WIG opc with the accumulator in VEX.vvvv.
             */
            if (in->opcode != NYR_MACH_ADD && in->opcode != NYR_MACH_SUB &&
                in->opcode != NYR_MACH_MUL && in->opcode != NYR_MACH_DIV)
              return mach_err(
                  e, "x64 machine form encode: no packed f64 bitwise/shift");
            unsigned char opc = in->opcode == NYR_MACH_ADD   ? 0x58
                                : in->opcode == NYR_MACH_SUB ? 0x5c
                                : in->opcode == NYR_MACH_MUL ? 0x59
                                                             : 0x5e;
            if (!mach_u8(e, 0xc4) || !mach_u8(e, 0xe1) || !mach_u8(e, 0x7e) ||
                !mach_u8(e, 0x6f) || !mach_u8(e, 0x85) || !mach_i32(e, a) ||
                !mach_u8(e, 0xc4) || !mach_u8(e, 0xe1) || !mach_u8(e, 0x7d) ||
                !mach_u8(e, opc) || !mach_u8(e, 0x85) || !mach_i32(e, b) ||
                !mach_u8(e, 0xc4) || !mach_u8(e, 0xe1) || !mach_u8(e, 0x7e) ||
                !mach_u8(e, 0x7f) || !mach_u8(e, 0x85) || !mach_i32(e, dst))
              return false;
          } else if (vt == NYR_MACH_TYPE_V128_F64) {
            /*
             * addpd/subpd/mulpd/divpd — 66 0F opc
             */
            if (in->opcode == NYR_MACH_AND || in->opcode == NYR_MACH_OR ||
                in->opcode == NYR_MACH_XOR || in->opcode == NYR_MACH_SHL ||
                in->opcode == NYR_MACH_SAR || in->opcode == NYR_MACH_ROR)
              return mach_err(
                  e, "x64 machine form encode: no packed f64 bitwise/shift");
            unsigned char opc = in->opcode == NYR_MACH_ADD   ? 0x58
                                : in->opcode == NYR_MACH_SUB ? 0x5c
                                : in->opcode == NYR_MACH_MUL ? 0x59
                                : in->opcode == NYR_MACH_DIV ? 0x5e
                                                             : 0xff;
            if (opc == 0xff || !mach_vec_op_mem_reg(e, 0, b, 0x66, opc) ||
                !mach_store_xmm0_v128(e, dst))
              return false;
          } else if (vt == NYR_MACH_TYPE_V128_F32) {
            /*
             * addps/subps/mulps/divps — 0F opc
             */
            if (in->opcode == NYR_MACH_AND || in->opcode == NYR_MACH_OR ||
                in->opcode == NYR_MACH_XOR || in->opcode == NYR_MACH_SHL ||
                in->opcode == NYR_MACH_SAR || in->opcode == NYR_MACH_ROR)
              return mach_err(
                  e, "x64 machine form encode: no packed f32 bitwise/shift");
            unsigned char opc = in->opcode == NYR_MACH_ADD   ? 0x58
                                : in->opcode == NYR_MACH_SUB ? 0x5c
                                : in->opcode == NYR_MACH_MUL ? 0x59
                                : in->opcode == NYR_MACH_DIV ? 0x5e
                                                             : 0xff;
            if (opc == 0xff || !mach_vec_op_mem_reg(e, 0, b, 0, opc) ||
                !mach_store_xmm0_v128(e, dst))
              return false;
          } else
            return mach_err(e, "x64 machine form encode: bad v128 ALU type");
          break;
        }
        if (in->opcode == NYR_MACH_SHL || in->opcode == NYR_MACH_SAR ||
            in->opcode == NYR_MACH_ROR || in->opcode == NYR_MACH_ROR32) {
          if (mach_is_v128(mach, &in->dst)) {
            if (!e->vec_fast_path)
              return mach_err(
                  e,
                  "x64 machine form encode: vector shift requires fast path");
            nyr_mach_type_t vt = mach_v128_type(mach, &in->dst);
            bool is_shift_left = in->opcode == NYR_MACH_SHL;
            unsigned prefix =
                (vt == NYR_MACH_TYPE_V128_I64 || vt == NYR_MACH_TYPE_V128_F64)
                    ? 0x66
                    : 0;
            unsigned opc;
            if (vt == NYR_MACH_TYPE_V128_I64) {
              /*
               * SSE2/AVX2 has NO packed-qword arithmetic right shift.
               * 66 0F E3 is PAVGW (defined, silently wrong); ROR's 0F 30
               * is WRMSR (#UD in ring 3). Reject both so callers fall
               * back instead of executing garbage.
               */
              if (!is_shift_left)
                return mach_err(
                    e, "x64 machine form encode: packed i64 arithmetic "
                       "right-shift has no SSE2/AVX2 form");
              opc = 0xf3; /* PSLLQ */
            } else if (vt == NYR_MACH_TYPE_V128_F64) {
              opc = is_shift_left ? 0xf3
                                  : (in->opcode == NYR_MACH_ROR ? 0x30 : 0xe3);
            } else if (vt == NYR_MACH_TYPE_V128_F32) {
              opc = is_shift_left ? 0xf2
                                  : (in->opcode == NYR_MACH_ROR ? 0x30 : 0xe2);
            } else {
              return mach_err(
                  e, "x64 machine form encode: unsupported vector shift type");
            }
            if (!mach_vector_shift(e, in, dst, prefix, opc))
              return false;
            break;
          }
          /*
           * Scalar 64-bit shift/rotate: dst = a (<op> count) with count in cl
           * or imm. The allocator excludes RCX from live segments spanning
           * variable shifts. Read the count through its current residency,
           * not its possibly unmaterialized canonical home.
           */
          if (!mach_is_i64(mach, &in->dst))
            return mach_err(
                e, "x64 machine form encode: scalar shift/rotate needs i64");
          bool is_ror32 = in->opcode == NYR_MACH_ROR32;
          unsigned op = in->opcode == NYR_MACH_SHL   ? 0xe0
                        : in->opcode == NYR_MACH_SAR ? 0xf8
                                                     : 0xc8;
          int64_t fold = 0;
          bool fold_ok = false;
          if (in->src1.kind == NYR_MACH_OPERAND_IMM) {
            fold = in->src1.as.imm;
            fold_ok = true;
          } else if (in->src1.kind == NYR_MACH_OPERAND_VREG && imm_fold_known &&
                     in->src1.as.reg < mach->vreg_len &&
                     imm_fold_known[in->src1.as.reg]) {
            fold = imm_fold[in->src1.as.reg];
            fold_ok = true;
          }
          if (getenv("NY_SHIFT_PREG") && fold_ok &&
              (in->opcode == NYR_MACH_SHL || in->opcode == NYR_MACH_SAR ||
               in->opcode == NYR_MACH_ROR || in->opcode == NYR_MACH_ROR32) &&
              in->dst.kind == NYR_MACH_OPERAND_VREG &&
              in->src0.kind == NYR_MACH_OPERAND_VREG) {
            /*
             * Experimental (NY_SHIFT_PREG=1): destination-preg immediate
             * shifts checksum-mismatch on sha256/pbkdf2 — some load channel
             * still disagrees with the %rax path.  Kept behind the gate
             * until root-caused; do not enable by default.
             *
             * Destination-preg immediate shift: stage the canonical home
             * straight into the destination register and shift there,
             * skipping the %rax round trip and the def publish.  ROR32
             * additionally truncates in place: the rotate runs 64-bit, so
             * a 32-bit self-move re-zero-extends the result exactly like
             * the %rax path's `mov %eax,%eax`.
             */
            int dp = mach_vreg_def_preg(e, in->dst.as.reg);
            if (dp >= 0) {
              if (!mach_load_src_preg(e, &in->src0, (unsigned)dp) ||
                  !mach_shift_preg_imm(e, is_ror32 ? NYR_MACH_ROR : in->opcode,
                                       dp, fold & (is_ror32 ? 31 : 63)))
                return false;
              if (is_ror32 &&
                  !(mach_rex(e, false, dp >= 8, false, dp >= 8) &&
                    mach_u8(e, 0x89) &&
                    mach_u8(e,
                            (unsigned char)(0xc0 | (dp & 7) << 3 | (dp & 7)))))
                return false;
              if (e->color_seeded) {
                for (uint32_t v = 0; v < e->colors_len; ++v) {
                  if (v == in->dst.as.reg || !e->color_seeded[v])
                    continue;
                  if (mach_vreg_preg(e, v) == dp)
                    e->color_seeded[v] = false;
                }
              }
              if (mach_vreg_requires_home_after_def(e, in->dst.as.reg)) {
                if (!mach_store_preg_home(
                        e, dp, mach_vreg_home_off(e, in->dst.as.reg)))
                  return false;
              }
              if (dp == 0)
                mach_rax_invalidate(e);
              if (e->color_seeded && in->dst.as.reg < e->colors_len)
                e->color_seeded[in->dst.as.reg] = true;
              break;
            }
          }
          if (fold_ok) {
            /*
             * x86 masks shift counts architecturally. Fold the same rule
             * here for negative and oversized constants instead of routing
             * them through CL, where call-clobber and register allocation
             * can otherwise change the observed result.
             */
            fold &= is_ror32 ? 31 : 63;
            /*
             * A shifted index whose only consumer is the address ADD of a
             * pending SIB access needs no bytes of its own: the access's
             * staging sequence rebuilds (input << bits) in the scratch pair
             * and both the shift and the ADD are elided.
             */
            if (!mach_load_op_rax(e, &in->src0) ||
                (!is_ror32 && !mach_rex(e, true, false, false, false)) ||
                !mach_u8(e, 0xc1) || !mach_u8(e, op) ||
                !mach_u8(e, (unsigned char)(fold & 0xff)) ||
                (is_ror32 && (!mach_u8(e, 0x89) || !mach_u8(e, 0xc0))))
              return false;
          } else {
            if (!mach_load_src_preg(e, &in->src1, 1) ||
                !mach_load_op_rax(e, &in->src0) ||
                (!is_ror32 && !mach_rex(e, true, false, false, false)) ||
                !mach_u8(e, 0xd3) || !mach_u8(e, op) ||
                (is_ror32 && (!mach_u8(e, 0x89) || !mach_u8(e, 0xc0))))
              return false;
          }
          if (!mach_commit_vreg(e, &in->dst, dst))
            return false;
          break;
        }
        if (in->opcode == NYR_MACH_DIV)
          goto mach_div_scalar;
        if (mach_is_float(mach, &in->dst)) {
          bool f32 = mach_is_f32(mach, &in->dst);
          unsigned opc = in->opcode == NYR_MACH_ADD   ? 0x58
                         : in->opcode == NYR_MACH_SUB ? 0x5c
                         : in->opcode == NYR_MACH_MUL ? 0x59
                                                      : 0xff;
          if (opc == 0xff || in->opcode == NYR_MACH_AND ||
              in->opcode == NYR_MACH_OR || in->opcode == NYR_MACH_XOR)
            return mach_err(e, "x64 machine form encode: bad float ALU");
          if (e->fp_fast_path) {
            if (!mach_float_binop(e, in, dst, f32, opc))
              return false;
          } else if (!mach_load_xmm0(e, a, f32) ||
                     !mach_sse_op_mem(e, b, f32, opc) ||
                     !mach_store_xmm0(e, dst, f32))
            return false;
          break;
        }
        if (!mach_is_i64(mach, &in->dst))
          return mach_err(e, "x64 machine form encode: unsupported ALU type");
        if (in->dst.kind == NYR_MACH_OPERAND_VREG &&
            in->src0.kind == NYR_MACH_OPERAND_VREG) {
          int dp = mach_vreg_def_preg(e, in->dst.as.reg);
          /*
           * Cross-block values keep a canonical home but are not resident in
           * this block until seeded. Seed before querying their usable preg;
           * the generic RAX path would otherwise reload the same home for
           * each arithmetic consumer.
           */
          if (dp >= 0 && !mach_seed_colored_preg(e, in->src0.as.reg))
            return false;
          int ap = mach_vreg_preg(e, in->src0.as.reg);
          if (dp >= 0 && ap >= 0) {
            int64_t fold = 0;
            bool fold_ok = false;
            if (in->src1.kind == NYR_MACH_OPERAND_IMM) {
              fold = in->src1.as.imm;
              fold_ok = (int64_t)(int32_t)fold == fold;
            } else if (in->src1.kind == NYR_MACH_OPERAND_VREG &&
                       imm_fold_known && in->src1.as.reg < mach->vreg_len &&
                       imm_fold_known[in->src1.as.reg]) {
              fold = imm_fold[in->src1.as.reg];
              fold_ok = (int64_t)(int32_t)fold == fold;
            }
            bool emitted = false;
            if (fold_ok) {
              unsigned scale = in->opcode == NYR_MACH_MUL ? (fold == 3   ? 1
                                                             : fold == 5 ? 2
                                                             : fold == 9 ? 3
                                                                         : 0)
                                                          : 0;
              if (in->opcode == NYR_MACH_ADD && dp != ap) {
                /* lea dst,[src+imm] is the three-operand form of a wrapping
                 * sum. Encode the SIB explicitly for RSP/R12 bases. */
                unsigned mod = fold >= INT8_MIN && fold <= INT8_MAX ? 1u : 2u;
                unsigned base = (unsigned)ap & 7u;
                if (!mach_rex(e, true, dp >= 8, false, ap >= 8) ||
                    !mach_u8(e, 0x8d) ||
                    !mach_u8(e,
                             (mod << 6) | (((unsigned)dp & 7u) << 3) | base) ||
                    (base == 4 && !mach_u8(e, 0x24)) ||
                    !(mod == 1 ? mach_u8(e, (unsigned char)fold)
                               : mach_i32(e, (int32_t)fold)))
                  return false;
                emitted = true;
              } else if (scale) {
                if (!mach_mul_preg_imm_from_preg(e, dp, ap, fold))
                  return false;
                emitted = true;
              } else {
                if (!mach_mov_preg_preg(e, dp, ap) ||
                    !mach_alu_preg_imm(e, in->opcode, dp, fold))
                  return false;
                emitted = true;
              }
            } else if (in->src1.kind == NYR_MACH_OPERAND_VREG) {
              if (!mach_seed_colored_preg(e, in->src1.as.reg))
                return false;
              int bp = mach_vreg_preg(e, in->src1.as.reg);
              if (bp >= 0) {
                /* A three-register sum needs no copy before the ADD.
                 * Machine arithmetic has no implicit flag consumer; compare
                 * fusion establishes its own flags at the later CMP. */
                if (in->opcode == NYR_MACH_ADD && dp != ap && dp != bp &&
                    bp != 4) {
                  ny_x64_mem_addr_t sum = {(unsigned)ap, bp, 1, 0};
                  if (!mach_rex_for_addr(e, &sum, true, dp >= 8) ||
                      !mach_u8(e, 0x8d) ||
                      !mach_emit_mem(e, &sum, (unsigned)dp))
                    return false;
                  emitted = true;
                } else {
                  bool commutative = in->opcode != NYR_MACH_SUB;
                  int rhs = bp;
                  if (dp != ap) {
                    if (bp == dp) {
                      if (commutative)
                        rhs = ap;
                      else
                        rhs = -1;
                    } else if (!mach_mov_preg_preg(e, dp, ap)) {
                      return false;
                    }
                  }
                  if (rhs >= 0) {
                    if (!mach_alu_preg_preg(e, in->opcode, dp, rhs))
                      return false;
                    emitted = true;
                  }
                }
              }
            }
            if (emitted) {
              /*
               * The direct ALU path can define dst in a preg still marked
               * resident for another live vreg (notably an in-place update
               * of a value that remains live for a later instruction). The
               * value's home remains authoritative after this write, so
               * force those later uses to reload instead of trusting the
               * overwritten preg.
               */
              if (e->color_seeded) {
                for (uint32_t v = 0; v < e->colors_len; ++v) {
                  if (v == in->dst.as.reg || !e->color_seeded[v])
                    continue;
                  if (mach_vreg_preg(e, v) == dp)
                    e->color_seeded[v] = false;
                }
              }
              if (mach_vreg_requires_home_after_def(e, in->dst.as.reg)) {
                if (!mach_store_preg_home(
                        e, dp, mach_vreg_home_off(e, in->dst.as.reg)))
                  return false;
                mach_rax_invalidate(e);
              } else if (dp == 0) {
                /*
                 * The direct ALU path updates `%rax` without going through
                 * mach_commit_vreg. Clear the sticky stack-home cache so a
                 * later mach_load_op_rax cannot reuse a pre-ALU value.
                 */
                mach_rax_invalidate(e);
              }
              if (e->color_seeded && in->dst.as.reg < e->colors_len)
                e->color_seeded[in->dst.as.reg] = true;
              break;
            }
          }
          if (dp >= 0 && ap < 0) {
            /*
             * Uncolored source: pull its canonical home straight into the
             * destination preg and apply the operation there.  This skips
             * the %rax stage and the def-side publish for chain-heavy
             * integer code (hash rounds, PRNG mixing, PRNG-like loops).
             */
            bool emitted2 = false;
            if (!mach_load_src_preg(e, &in->src0, (unsigned)dp))
              return false;
            int64_t fold = 0;
            bool fold_ok = false;
            if (in->src1.kind == NYR_MACH_OPERAND_IMM) {
              fold = in->src1.as.imm;
              fold_ok = (int64_t)(int32_t)fold == fold;
            } else if (in->src1.kind == NYR_MACH_OPERAND_VREG &&
                       imm_fold_known && in->src1.as.reg < mach->vreg_len &&
                       imm_fold_known[in->src1.as.reg]) {
              fold = imm_fold[in->src1.as.reg];
              fold_ok = (int64_t)(int32_t)fold == fold;
            }
            if (fold_ok) {
              emitted2 = mach_alu_preg_imm(e, in->opcode, dp, fold);
            } else if (in->src1.kind == NYR_MACH_OPERAND_VREG) {
              if (!mach_seed_colored_preg(e, in->src1.as.reg))
                return false;
              int bp = mach_vreg_preg(e, in->src1.as.reg);
              if (bp >= 0 && bp != dp) {
                emitted2 = mach_alu_preg_preg(e, in->opcode, dp, bp);
              } else if (bp < 0) {
                /*
                 * Uncolored right-hand side: bring it through the trusted
                 * load into %rax, then fold it in as a register operand.
                 */
                emitted2 = mach_load_op_rax(e, &in->src1) &&
                           mach_alu_preg_preg(e, in->opcode, dp, 0);
              }
              /*
               * bp == dp (shared color across a def) is rare; leave it to
               * the generic path rather than reasoning about clobber order.
               */
            }
            if (emitted2) {
              if (e->color_seeded) {
                for (uint32_t v = 0; v < e->colors_len; ++v) {
                  if (v == in->dst.as.reg || !e->color_seeded[v])
                    continue;
                  if (mach_vreg_preg(e, v) == dp)
                    e->color_seeded[v] = false;
                }
              }
              if (mach_vreg_requires_home_after_def(e, in->dst.as.reg)) {
                if (!mach_store_preg_home(
                        e, dp, mach_vreg_home_off(e, in->dst.as.reg)))
                  return false;
                mach_rax_invalidate(e);
              } else if (dp == 0) {
                mach_rax_invalidate(e);
              }
              if (e->color_seeded && in->dst.as.reg < e->colors_len)
                e->color_seeded[in->dst.as.reg] = true;
              break;
            }
          }
        }
        /*
         * Keep a loop-carried multiply/add chain in its resident preg when
         * both intermediates have one immediate src0 consumer.  This is the
         * scalar form of x *= K; x += C before a modulus: it avoids moving
         * the carried state into %rax and avoids two transient stack homes.
         */
        if (in->opcode == NYR_MACH_MUL &&
            in->src0.kind == NYR_MACH_OPERAND_VREG &&
            in->dst.kind == NYR_MACH_OPERAND_VREG && n + 2 < blk->inst_count) {
          int preg = mach_vreg_preg(e, in->src0.as.reg);
          const nyr_mach_live_segment_t *src_seg =
              mach_segment_at(e, in->src0.as.reg, e->cur_inst);
          size_t next_index = e->cur_inst + 1;
          const nyr_mach_inst_t *next = &mach->insts[next_index];
          const nyr_mach_inst_t *consumer = &mach->insts[next_index + 1];
          int64_t mul_imm = 0, add_imm = 0;
          bool mul_const = false, add_const = false, src_reused = false;
          if (in->src1.kind == NYR_MACH_OPERAND_IMM) {
            mul_imm = in->src1.as.imm;
            mul_const = (int64_t)(int32_t)mul_imm == mul_imm;
          } else if (in->src1.kind == NYR_MACH_OPERAND_VREG && imm_fold_known &&
                     in->src1.as.reg < mach->vreg_len &&
                     imm_fold_known[in->src1.as.reg]) {
            mul_imm = imm_fold[in->src1.as.reg];
            mul_const = (int64_t)(int32_t)mul_imm == mul_imm;
          }
          if (next->src1.kind == NYR_MACH_OPERAND_IMM) {
            add_imm = next->src1.as.imm;
            add_const = (int64_t)(int32_t)add_imm == add_imm;
          } else if (next->src1.kind == NYR_MACH_OPERAND_VREG &&
                     imm_fold_known && next->src1.as.reg < mach->vreg_len &&
                     imm_fold_known[next->src1.as.reg]) {
            add_imm = imm_fold[next->src1.as.reg];
            add_const = (int64_t)(int32_t)add_imm == add_imm;
          }
          for (size_t i = next_index + 1; i < blk->first_inst + blk->inst_count;
               ++i)
            if (mach_inst_uses_vreg(&mach->insts[i], in->src0.as.reg)) {
              src_reused = true;
              break;
            }
          if (preg >= 0 && src_seg && src_seg->carried && mul_const &&
              add_const && !src_reused && next->opcode == NYR_MACH_ADD &&
              next->dst.kind == NYR_MACH_OPERAND_VREG &&
              next->src0.kind == NYR_MACH_OPERAND_VREG &&
              next->src0.as.reg == in->dst.as.reg &&
              consumer->src0.kind == NYR_MACH_OPERAND_VREG &&
              consumer->src0.as.reg == next->dst.as.reg &&
              !mach_use_requires_home(mach, consumer, next->dst.as.reg)) {
            bool later_use = false;
            for (size_t i = next_index + 2; i < mach->inst_len; ++i)
              if (mach_inst_uses_vreg(&mach->insts[i], next->dst.as.reg)) {
                later_use = true;
                break;
              }
            if (!later_use) {
              unsigned char modrm =
                  (unsigned char)(0xc0 | ((preg & 7) << 3) | (preg & 7));
              if (!mach_rex(e, true, preg >= 8, false, preg >= 8) ||
                  !mach_u8(e, 0x69) || !mach_u8(e, modrm) ||
                  !mach_i32(e, (int32_t)mul_imm) ||
                  !mach_rex(e, true, false, false, preg >= 8) ||
                  !mach_u8(e, 0x8d) ||
                  !mach_u8(e, (unsigned char)(0x80 | (preg & 7))) ||
                  !mach_i32(e, (int32_t)add_imm))
                return false;
              e->pending_rax_off = mach_slot_off(e, &next->dst);
              e->pending_rax_valid = true;
              e->cur_inst = next_index;
              ++n;
              break;
            }
          }
        }
        if (e->pending_rax_valid)
          break;
        if (!mach_load_op_rax(e, &in->src0))
          return false;
        /*
         * Dedicated loop registers stay valid while RAX holds the left
         * operand. Consume the resident right operand directly as well.
         */
        if (in->src1.kind == NYR_MACH_OPERAND_VREG && e->loop_gpr &&
            e->loop_gpr[in->src1.as.reg] >= 0) {
          int rhs = mach_vreg_preg(e, in->src1.as.reg);
          if (rhs >= 0) {
            if (!mach_alu_preg_preg(e, in->opcode, 0, rhs) ||
                !mach_commit_vreg(e, &in->dst, dst))
              return false;
            break;
          }
        }
        {
          /*
           * Fold COPY-from-IMM src1 into an immediate operand when the value
           * fits a sign-extended imm32; otherwise use the memory-operand
           * form. Accumulator forms (05/2d/25/0d/35) and imul rax,rax,imm32
           * (69 c0) both sign-extend imm32 to 64 bits.
           */
          int64_t fold = 0;
          bool fold_ok = false;
          if (in->src1.kind == NYR_MACH_OPERAND_VREG && imm_fold_known &&
              in->src1.as.reg < mach->vreg_len &&
              imm_fold_known[in->src1.as.reg]) {
            fold = imm_fold[in->src1.as.reg];
            fold_ok = (int64_t)(int32_t)fold == fold;
          }
          if (fold_ok) {
            unsigned char op = 0;
            if (in->opcode == NYR_MACH_ADD)
              op = 0x05; /* add rax, imm32 */
            else if (in->opcode == NYR_MACH_SUB)
              op = 0x2d;
            else if (in->opcode == NYR_MACH_AND)
              op = 0x25;
            else if (in->opcode == NYR_MACH_OR)
              op = 0x0d;
            else if (in->opcode == NYR_MACH_XOR)
              op = 0x35;
            if (op) {
              if (!mach_rex(e, true, false, false, false) || !mach_u8(e, op) ||
                  !mach_i32(e, (int32_t)fold) ||
                  !mach_commit_vreg(e, &in->dst, dst))
                return false;
              break;
            }
            /*
             * Small odd constant multiplies are address arithmetic on x86.
             * LEA avoids the multiply unit and shortens x*3/x*5/x*9 to four
             * bytes; preserve the general immediate multiply otherwise.
             */
            unsigned scale = fold == 3 ? 1 : fold == 5 ? 2 : fold == 9 ? 3 : 0;
            if (in->opcode == NYR_MACH_MUL && scale) {
              if (!mach_rex(e, true, false, false, false) ||
                  !mach_u8(e, 0x8d) || !mach_u8(e, 0x04) ||
                  !mach_u8(e, (scale << 6)) ||
                  !mach_commit_vreg(e, &in->dst, dst))
                return false;
              break;
            }
            /*
             * MUL imm32: imul rax, rax, imm32 (sign-extended).
             */
            if (!mach_rex(e, true, false, false, false) || !mach_u8(e, 0x69) ||
                !mach_u8(e, 0xc0) || !mach_i32(e, (int32_t)fold) ||
                !mach_commit_vreg(e, &in->dst, dst))
              return false;
            break;
          }
          unsigned char op = 0;
          if (in->opcode == NYR_MACH_ADD)
            op = 0x03;
          else if (in->opcode == NYR_MACH_SUB)
            op = 0x2b;
          else if (in->opcode == NYR_MACH_AND)
            op = 0x23;
          else if (in->opcode == NYR_MACH_OR)
            op = 0x0b;
          else if (in->opcode == NYR_MACH_XOR)
            op = 0x33;
          else {
            if (!mach_rex(e, true, false, false, false) || !mach_u8(e, 0x0f) ||
                !mach_u8(e, 0xaf) || !mach_u8(e, 0x85) || !mach_i32(e, b) ||
                !mach_commit_vreg(e, &in->dst, dst))
              return false;
            break;
          }
          if (!mach_rex(e, true, false, false, false) || !mach_u8(e, op) ||
              !mach_u8(e, 0x85) || !mach_i32(e, b) ||
              !mach_commit_vreg(e, &in->dst, dst))
            return false;
        }
        break;
      case NYR_MACH_FMA: {
        /*
         * dst = a*b + c  (mul + add; true FMA when available is a later
         * pass).
         */
        int c_off = mach_slot_off(e, &in->src2);
        if (mach_is_v128(mach, &in->dst)) {
          nyr_mach_type_t vt = mach_v128_type(mach, &in->dst);
          if (e->vec_fast_path) {
            if (vt == NYR_MACH_TYPE_V256_F64) {
              /*
               * Lowering admits four-lane f64 FMA only on fused-FMA
               * targets, so this encodes the single-rounding vfmadd213pd.
               */
              if (!mach_vector_ymm_fma(e, in, dst))
                return false;
            } else if (vt == NYR_MACH_TYPE_V128_F64) {
              if (!mach_vector_fma(e, in, dst, 0x66, 0x59, 0x58))
                return false;
            } else if (vt == NYR_MACH_TYPE_V128_F32) {
              if (!mach_vector_fma(e, in, dst, 0, 0x59, 0x58))
                return false;
            } else {
              return mach_err(e, "x64 machine form encode: vector FMA type");
            }
            break;
          }
          if (vt == NYR_MACH_TYPE_V256_F64) {
            if (!mach_ymm_load_v256(e, a, 14) ||
                !mach_ymm_load_v256(e, b, 15) ||
                !mach_ymm_vfmadd213_mem(e, 14, 15, c_off) ||
                !mach_ymm_store_v256(e, dst, 14))
              return false;
            break;
          }
          if (!mach_load_xmm0_v128(e, a))
            return false;
          if (vt == NYR_MACH_TYPE_V128_F64) {
            if (!mach_vec_op_mem_reg(e, 0, b, 0x66, 0x59) ||
                !mach_vec_op_mem_reg(e, 0, c_off, 0x66, 0x58) ||
                !mach_store_xmm0_v128(e, dst))
              return false;
          } else if (vt == NYR_MACH_TYPE_V128_F32) {
            if (!mach_vec_op_mem_reg(e, 0, b, 0, 0x59) ||
                !mach_vec_op_mem_reg(e, 0, c_off, 0, 0x58) ||
                !mach_store_xmm0_v128(e, dst))
              return false;
          } else
            return mach_err(e,
                            "x64 machine form encode: FMA needs f32/f64 v128");
          break;
        }
        if (mach_is_float(mach, &in->dst)) {
          bool f32 = mach_is_f32(mach, &in->dst);
          if (e->fp_fast_path) {
            int dreg = in->dst.kind == NYR_MACH_OPERAND_VREG
                           ? mach_vreg_fpreg(e, in->dst.as.reg)
                           : -1;
            if (!mach_float_binop(e, in, dst, f32, 0x59))
              return false;
            if (dreg < 0 && !mach_load_xmm(e, dst, f32, 14))
              return false;
            if (!mach_sse_op_mem_reg(e, dreg >= 0 ? (unsigned)dreg : 14, c_off,
                                     f32, 0x58))
              return false;
            if (dreg < 0 && !mach_store_xmm(e, dst, f32, 14))
              return false;
          } else if (!mach_load_xmm0(e, a, f32) ||
                     !mach_sse_op_mem(e, b, f32, 0x59) ||
                     !mach_sse_op_mem(e, c_off, f32, 0x58) ||
                     !mach_store_xmm0(e, dst, f32))
            return false;
          break;
        }
        return mach_err(e, "x64 machine form encode: unsupported FMA type");
      }
      case NYR_MACH_SHUFFLE: {
        if (!mach_is_v128(mach, &in->dst) ||
            in->src1.kind != NYR_MACH_OPERAND_IMM)
          return mach_err(e, "x64 machine form encode: bad shuffle");
        nyr_mach_type_t vt = mach_v128_type(mach, &in->dst);
        unsigned imm = (unsigned)(in->src1.as.imm & 0xff);
        if (e->vec_fast_path) {
          nyr_mach_type_t fast_vt = mach_v128_type(mach, &in->dst);
          int assigned = in->dst.kind == NYR_MACH_OPERAND_VREG
                             ? mach_vreg_vpreg(e, in->dst.as.reg)
                             : -1;
          unsigned d = assigned >= 0 ? (unsigned)assigned : 14;
          unsigned s = d == 14 ? 15 : 14;
          if (!mach_load_vector_operand(e, &in->src0, s, &s))
            return false;
          if (s != d && !mach_vec_mov_reg(e, d, s))
            return false;
          if (fast_vt == NYR_MACH_TYPE_V128_F64) {
            if (!mach_u8(e, 0x66) || !mach_u8(e, 0x0f) || !mach_u8(e, 0xc6) ||
                !mach_u8(e, 0xc0 | ((d & 7) << 3) | (d & 7)) ||
                !mach_u8(e, imm & 3))
              return false;
          } else if (vt == NYR_MACH_TYPE_V128_F32) {
            if (!mach_u8(e, 0x0f) || !mach_u8(e, 0xc6) ||
                !mach_u8(e, 0xc0 | ((d & 7) << 3) | (d & 7)) ||
                !mach_u8(e, imm))
              return false;
          } else {
            return mach_err(e, "x64 machine form encode: vector shuffle type");
          }
          if (assigned >= 0) {
            if (e->vec_color_seeded)
              e->vec_color_seeded[in->dst.as.reg] = true;
            break;
          }
          if (!mach_store_xmm_v128(e, dst, d))
            return false;
          break;
        }
        if (vt == NYR_MACH_TYPE_V256_F64) {
          /*
           * General four-lane permutation. Fast-path eligibility excludes
           * this shape, so the destination is always a home and xmm0-xmm3
           * carry no colored vector segments. Extract both 128-bit halves,
           * then build each destination half from two lane broadcasts.
           */
          unsigned v = 15;
          if (!mach_load_vector_operand(e, &in->src0, 14, &v))
            return false;
          if (v != 15 && !mach_ymm_mov_reg(e, 15, v))
            return false;
          if (!mach_xmm_vextractf128(e, 1, 15, 0) ||
              !mach_xmm_vextractf128(e, 2, 15, 1))
            return false;
          for (unsigned half = 0; half < 2; ++half) {
            unsigned p = (imm >> ((half * 2u) * 2u)) & 3u;
            unsigned q = (imm >> ((half * 2u + 1u) * 2u)) & 3u;
            unsigned src_p = p >> 1u ? 2u : 1u;
            unsigned src_q = q >> 1u ? 2u : 1u;
            /*
             * T = broadcast of lane p into both xmm0 slots.
             */
            if (p & 1u) {
              if (!mach_xmm_movaps(e, 0, src_p) || !mach_xmm_unpckhpd(e, 0, 0))
                return false;
            } else if (!mach_xmm_movddup(e, 0, src_p))
              return false;
            /*
             * U = broadcast of lane q into both xmm3 slots.
             */
            if (q & 1u) {
              if (!mach_xmm_movaps(e, 3, src_q) || !mach_xmm_unpckhpd(e, 3, 3))
                return false;
            } else if (!mach_xmm_movddup(e, 3, src_q))
              return false;
            /*
             * dest half = {T[0], U[1]}; store it at its home half.
             */
            if (!mach_xmm_shufpd(e, 0, 3, 0x2) ||
                !mach_xmm_movupd_store_mem(e, 0, half == 0 ? dst : dst + 16))
              return false;
          }
          break;
        }
        if (!mach_load_xmm0_v128(e, a))
          return false;
        if (vt == NYR_MACH_TYPE_V128_F64) {
          /*
           * shufpd imm: 66 0F C6 C0 ib
           */
          if (!mach_u8(e, 0x66) || !mach_u8(e, 0x0f) || !mach_u8(e, 0xc6) ||
              !mach_u8(e, 0xc0) || !mach_u8(e, (unsigned char)(imm & 3)) ||
              !mach_store_xmm0_v128(e, dst))
            return false;
        } else if (vt == NYR_MACH_TYPE_V128_F32) {
          /*
           * shufps imm: 0F C6 C0 ib
           */
          if (!mach_u8(e, 0x0f) || !mach_u8(e, 0xc6) || !mach_u8(e, 0xc0) ||
              !mach_u8(e, (unsigned char)imm) || !mach_store_xmm0_v128(e, dst))
            return false;
        } else
          return mach_err(e, "x64 machine form encode: shuffle needs f32/f64");
        break;
      }
      case NYR_MACH_MOD:
      mach_div_scalar:
        if (mach_is_v128(mach, &in->dst) && in->opcode == NYR_MACH_DIV)
          return mach_err(e, "x64 machine form encode: v128 div handled above");
        if (mach_is_float(mach, &in->dst) && in->opcode == NYR_MACH_DIV) {
          bool f32 = mach_is_f32(mach, &in->dst);
          if (e->fp_fast_path) {
            if (!mach_float_binop(e, in, dst, f32, 0x5e))
              return false;
          } else if (!mach_load_xmm0(e, a, f32) ||
                     !mach_sse_op_mem(e, b, f32, 0x5e) ||
                     !mach_store_xmm0(e, dst, f32))
            return false;
          break;
        }
        if (!mach_is_i64(mach, &in->dst))
          return mach_err(e, "x64 machine form encode: unsupported div type");
        unsigned pow2_shift = 0;
        if ((in->opcode == NYR_MACH_DIV || in->opcode == NYR_MACH_MOD) &&
            mach_pow2_divisor(&in->src1, imm_fold, imm_fold_known,
                              mach->vreg_len, &pow2_shift)) {
          uint32_t mask = ((uint32_t)1 << pow2_shift) - 1;
          /*
           * For signed truncation, add (2^shift - 1) to every negative
           * dividend before arithmetic shift. This also gives the right
           * quotient for exact negative multiples; the addition cannot
           * overflow because the bias is at most INT64_MAX.
           */
          unsigned pow2_clobbers = (1u << 0) | (1u << 1) | (1u << 2);
          /*
           * A loop-carried dividend may already be valid in a non-scratch
           * colored register. Preserve it as the input to the truncation
           * bias sequence instead of staging it through RAX. Besides saving
           * a move, this lets LEA combine the dividend and bias directly.
           */
          int direct_dividend_preg = -1;
          if (in->opcode == NYR_MACH_DIV &&
              in->src0.kind == NYR_MACH_OPERAND_VREG) {
            int preg = mach_vreg_preg(e, in->src0.as.reg);
            if (preg >= 0 && !(pow2_clobbers & (1u << preg)))
              direct_dividend_preg = preg;
          }
          if (!mach_flush_pregs_to_home(e, pow2_clobbers))
            return false;
          bool dividend_loaded = false;
          if (direct_dividend_preg >= 0) {
            dividend_loaded = true;
          } else if (in->src0.kind == NYR_MACH_OPERAND_VREG) {
            int dividend_preg = mach_vreg_preg(e, in->src0.as.reg);
            if (dividend_preg >= 0 && !(pow2_clobbers & (1u << dividend_preg)))
              dividend_loaded = mach_load_op_rax(e, &in->src0);
            else
              /*
               * The clobbered colored values were already published above.
               * Read the canonical home directly to avoid reseeding a
               * fixed-register color immediately before the shift sequence.
               */
              dividend_loaded = mach_load_rax(e, a);
          } else {
            dividend_loaded = mach_load_op_rax(e, &in->src0);
          }
          if (!dividend_loaded)
            return false;
          if (in->opcode == NYR_MACH_DIV) {
            if (direct_dividend_preg >= 0) {
              /*
               * q = (x + ((x >> 63) & (2^k - 1))) >> k, with truncation
               * toward zero. RDX is a flushed scratch register, while x
               * remains live in its assigned preg. LEA replaces the usual
               * copy-to-RAX plus ADD pair.
               */
              if (!mach_mov_preg_preg(e, 2, direct_dividend_preg) ||
                  !mach_rex(e, true, false, false, false) ||
                  !mach_u8(e, 0xc1) ||
                  !mach_u8(e, pow2_shift == 1 ? 0xea : 0xfa) || !mach_u8(e, 63))
                return false;
              if (pow2_shift > 1 && (!mach_rex(e, true, false, false, false) ||
                                     !mach_u8(e, 0x81) || !mach_u8(e, 0xe2) ||
                                     !mach_i32(e, (int32_t)mask)))
                return false;
              /*
               * lea rax, [direct_dividend_preg + rdx]. In SIB addressing,
               * base encodings 5 (RBP/R13) with mod=00 mean no base and
               * require a disp32. Use mod=01 plus disp8=0 for those bases.
               */
              bool needs_zero_disp8 = (direct_dividend_preg & 7) == 5;
              if (!mach_rex(e, true, false, false, direct_dividend_preg >= 8) ||
                  !mach_u8(e, 0x8d) ||
                  !mach_u8(e, needs_zero_disp8 ? 0x44 : 0x04) ||
                  !mach_u8(e, (unsigned char)(0x10 | (2 << 3) |
                                              (direct_dividend_preg & 7))) ||
                  (needs_zero_disp8 && !mach_u8(e, 0)) ||
                  !mach_rex(e, true, false, false, false) ||
                  !mach_u8(e, 0xc1) || !mach_u8(e, 0xf8) ||
                  !mach_u8(e, pow2_shift) ||
                  !mach_commit_vreg(e, &in->dst, dst))
                return false;
              break;
            }
            if (!mach_mov_preg_preg(e, 2, 0))
              return false;
            if (pow2_shift == 1) {
              /*
               * For division by two, the signed-truncation bias is exactly
               * the dividend's sign bit.  A logical shift computes that
               * 0/1 value directly, avoiding SAR plus AND.
               */
              if (!mach_rex(e, true, false, false, false) ||
                  !mach_u8(e, 0xc1) || !mach_u8(e, 0xea) || !mach_u8(e, 63))
                return false;
            } else if (!mach_rex(e, true, false, false, false) ||
                       !mach_u8(e, 0xc1) || !mach_u8(e, 0xfa) ||
                       !mach_u8(e, 63) ||
                       !mach_rex(e, true, false, false, false) ||
                       !mach_u8(e, 0x81) || !mach_u8(e, 0xe2) ||
                       !mach_i32(e, (int32_t)mask)) {
              return false;
            }
            if (!mach_rex(e, true, false, false, false) || !mach_u8(e, 0x01) ||
                !mach_u8(e, 0xd0) || !mach_rex(e, true, false, false, false) ||
                !mach_u8(e, 0xc1) || !mach_u8(e, 0xf8) ||
                !mach_u8(e, pow2_shift) || !mach_commit_vreg(e, &in->dst, dst))
              return false;
          } else if (pow2_shift == 1) {
            /*
             * Signed remainder by two is the low bit, negated for negative
             * odd dividends. Keep the original sign in RDX and form the
             * correction mask without a hardware divide.
             */
            if (!mach_mov_preg_preg(e, 2, 0) ||
                !mach_rex(e, true, false, false, false) || !mach_u8(e, 0x83) ||
                !mach_u8(e, 0xe0) || !mach_u8(e, 1) ||
                !mach_rex(e, true, false, false, false) || !mach_u8(e, 0xc1) ||
                !mach_u8(e, 0xfa) || !mach_u8(e, 63) ||
                !mach_rex(e, true, false, false, false) || !mach_u8(e, 0x8d) ||
                !mach_u8(e, 0x0c) || !mach_u8(e, 0x00) ||
                !mach_rex(e, true, false, false, false) || !mach_u8(e, 0x21) ||
                !mach_u8(e, 0xd1) || !mach_rex(e, true, false, false, false) ||
                !mach_u8(e, 0x29) || !mach_u8(e, 0xc8) ||
                !mach_commit_vreg(e, &in->dst, dst))
              return false;
          } else {
            if (!mach_rex(e, true, false, false, false) || !mach_u8(e, 0x89) ||
                !mach_u8(e, 0xc1) || !mach_rex(e, true, false, false, false) ||
                !mach_u8(e, 0x89) || !mach_u8(e, 0xc2) ||
                !mach_rex(e, true, false, false, false) || !mach_u8(e, 0xc1) ||
                !mach_u8(e, 0xfa) || !mach_u8(e, 63) ||
                !mach_rex(e, true, false, false, false) || !mach_u8(e, 0x81) ||
                !mach_u8(e, 0xe2) || !mach_i32(e, (int32_t)mask) ||
                !mach_rex(e, true, false, false, false) || !mach_u8(e, 0x01) ||
                !mach_u8(e, 0xd0) || !mach_rex(e, true, false, false, false) ||
                !mach_u8(e, 0xc1) || !mach_u8(e, 0xf8) ||
                !mach_u8(e, pow2_shift) ||
                !mach_rex(e, true, false, false, false) || !mach_u8(e, 0xc1) ||
                !mach_u8(e, 0xe0) || !mach_u8(e, pow2_shift) ||
                !mach_rex(e, true, false, false, false) || !mach_u8(e, 0x29) ||
                !mach_u8(e, 0xc1) || !mach_rex(e, true, false, false, false) ||
                !mach_u8(e, 0x89) || !mach_u8(e, 0xc8) ||
                !mach_commit_vreg(e, &in->dst, dst))
              return false;
          }
          break;
        }
        /*
         * Other integer divisions use fixed scratch registers below.
         */
        if (!mach_flush_pregs_to_home(e, (1u << 0) | (1u << 1) | (1u << 2) |
                                             (1u << 8) | (1u << 9) |
                                             (1u << 10)))
          return false;
        /*
         * Signed 64-bit remainder by 2^31-1 using the exact reciprocal
         * sequence selected by GCC. `imul` produces the high half needed for
         * the quotient; do not replace this with NYIR's low-half MUL.
         */
        int64_t divisor_imm = 0;
        bool mersenne31 =
            in->opcode == NYR_MACH_MOD &&
            in->src1.kind == NYR_MACH_OPERAND_VREG &&
            in->src1.as.reg < mach->vreg_len &&
            imm_fold_known[in->src1.as.reg] &&
            ((divisor_imm = imm_fold[in->src1.as.reg]) == INT64_C(2147483647));
        if (mersenne31) {
          /*
           * This sequence owns RAX, RDX, R8, and R9.  Values colored to
           * those registers must reach their canonical homes before the
           * sequence clobbers them; subsequent uses reload from those homes.
           */
          const unsigned mersenne_clobbers =
              (1u << 0) | (1u << 2) | (1u << 8) | (1u << 9);
          if (!mach_flush_pregs_to_home(e, mersenne_clobbers) ||
              !mach_load_op_rax(e, &in->src0) ||
              !mach_flush_pregs_to_home(e, mersenne_clobbers) ||
              /*
               * r9 = dividend; r8 = signed reciprocal.
               */
              !mach_rex(e, true, false, false, true) || !mach_u8(e, 0x89) ||
              !mach_u8(e, 0xc1) || !mach_rex(e, true, false, false, true) ||
              !mach_u8(e, 0xb8) ||
              !mach_i64(e, INT64_C(-9223372032559808509)) ||
              /*
               * rdx:rax = rax * r8; q = (rdx + dividend) >> 30 - sign.
               */
              !mach_rex(e, true, false, false, true) || !mach_u8(e, 0xf7) ||
              !mach_u8(e, 0xe8) || !mach_rex(e, true, true, false, false) ||
              !mach_u8(e, 0x01) || !mach_u8(e, 0xca) ||
              !mach_rex(e, true, false, false, false) || !mach_u8(e, 0xc1) ||
              !mach_u8(e, 0xfa) || !mach_u8(e, 30) ||
              !mach_rex(e, true, true, false, false) || !mach_u8(e, 0x89) ||
              !mach_u8(e, 0xc8) || !mach_rex(e, true, false, false, false) ||
              !mach_u8(e, 0xc1) || !mach_u8(e, 0xf8) || !mach_u8(e, 63) ||
              !mach_rex(e, true, false, false, false) || !mach_u8(e, 0x29) ||
              !mach_u8(e, 0xc2) ||
              /*
               * rax = q * (2^31 - 1); rax = dividend - rax.
               */
              !mach_rex(e, true, false, false, false) || !mach_u8(e, 0x89) ||
              !mach_u8(e, 0xd0) || !mach_rex(e, true, false, false, false) ||
              !mach_u8(e, 0xc1) || !mach_u8(e, 0xe0) || !mach_u8(e, 31) ||
              !mach_rex(e, true, false, false, false) || !mach_u8(e, 0x29) ||
              !mach_u8(e, 0xd0) || !mach_rex(e, true, false, false, true) ||
              !mach_u8(e, 0x29) || !mach_u8(e, 0xc1))
            return false;
          const nyr_mach_live_segment_t *dst_seg =
              in->dst.kind == NYR_MACH_OPERAND_VREG
                  ? mach_segment_at(e, in->dst.as.reg, e->cur_inst)
                  : NULL;
          if (dst_seg && dst_seg->carried) {
            int preg = mach_vreg_preg(e, in->dst.as.reg);
            if (preg < 0 || !mach_rex(e, true, true, false, preg >= 8) ||
                !mach_u8(e, 0x89) ||
                !mach_u8(e, (unsigned char)(0xc8 | (preg & 7))))
              return false;
          } else if (!mach_rex(e, true, true, false, false) ||
                     !mach_u8(e, 0x89) || !mach_u8(e, 0xc8) ||
                     !mach_commit_vreg(e, &in->dst, dst))
            return false;
          nyr_native_mach_div_record(true);
          break;
        }
        /*
         * Signed division/modulo by a non-power-of-2 compile-time constant
         * uses the Granlund-Montgomery magic-reciprocal sequence (the shape
         * GCC emits).  `imulq %r8` yields the high half of x*M, so this must
         * not be replaced by NYIR's low-half MUL.  Negative divisors divide
         * by |d| and negate the quotient; remainders use |d| directly because
         * x % d == x % |d|.
         */
        int64_t sdivisor = 0;
        bool sdivisor_known = false;
        if (in->src1.kind == NYR_MACH_OPERAND_IMM) {
          sdivisor = in->src1.as.imm;
          sdivisor_known = true;
        } else if (in->src1.kind == NYR_MACH_OPERAND_VREG && imm_fold &&
                   imm_fold_known && in->src1.as.reg < mach->vreg_len &&
                   imm_fold_known[in->src1.as.reg]) {
          sdivisor = imm_fold[in->src1.as.reg];
          sdivisor_known = true;
        }
        if (sdivisor_known && sdivisor != 0 && sdivisor != 1 &&
            sdivisor != -1) {
          uint64_t ad = sdivisor >= 0 ? (uint64_t)sdivisor
                                      : (uint64_t)(-(sdivisor + 1)) + 1;
          if ((ad & (ad - 1)) != 0) {
            int64_t magic = 0;
            unsigned mshift = 0;
            nyr_sdiv_magic_i64(ad, &magic, &mshift);
            bool negate = sdivisor < 0 && in->opcode == NYR_MACH_DIV;
            /*
             * Magic division uses several fixed scratch registers.
             */
            const unsigned magic_clobbers = (1u << 0) | (1u << 1) | (1u << 2) |
                                            (1u << 8) | (1u << 9) | (1u << 10);
            if (!mach_flush_pregs_to_home(e, magic_clobbers) ||
                !mach_load_op_rax(e, &in->src0) ||
                !mach_flush_pregs_to_home(e, magic_clobbers) ||
                !mach_mov_reg_rax(e, 9) ||
                !mach_rex(e, true, false, false, true) || !mach_u8(e, 0xb8) ||
                !mach_i64(e, magic) || !mach_rex(e, true, false, false, true) ||
                !mach_u8(e, 0xf7) || !mach_u8(e, 0xe8) ||
                !mach_mov_rax_reg(e, 2))
              return false;
            if (magic < 0 && !mach_alu_preg_preg(e, NYR_MACH_ADD, 0, 9))
              return false;
            if (mshift > 0 &&
                (!mach_rex(e, true, false, false, false) || !mach_u8(e, 0xc1) ||
                 !mach_u8(e, 0xf8) || !mach_u8(e, (unsigned char)mshift)))
              return false;
            if (!mach_mov_preg_preg(e, 1, 9) ||
                !mach_rex(e, true, false, false, false) || !mach_u8(e, 0xc1) ||
                !mach_u8(e, 0xf9) || !mach_u8(e, 63) ||
                !mach_alu_preg_preg(e, NYR_MACH_SUB, 0, 1))
              return false;
            if (negate && (!mach_rex(e, true, false, false, false) ||
                           !mach_u8(e, 0xf7) || !mach_u8(e, 0xd8)))
              return false;
            if (in->opcode == NYR_MACH_MOD) {
              if (!mach_mov_reg_rax(e, 8) || !mach_mov_rax_reg(e, 9))
                return false;
              if ((int64_t)(int32_t)ad == (int64_t)ad) {
                if (!mach_alu_preg_imm(e, NYR_MACH_MUL, 8, (int64_t)ad))
                  return false;
              } else if (!mach_rex(e, true, false, false, true) ||
                         !mach_u8(e, 0xba) || !mach_i64(e, (int64_t)ad) ||
                         !mach_alu_preg_preg(e, NYR_MACH_MUL, 8, 10)) {
                return false;
              }
              if (!mach_alu_preg_preg(e, NYR_MACH_SUB, 0, 8))
                return false;
            }
            if (!mach_commit_vreg(e, &in->dst, dst))
              return false;
            nyr_native_mach_div_record(true);
            break;
          }
        }
        /*
         * Both operands must use the allocator-aware loader.  A constant
         * divisor can be rematerialized and therefore need not have a
         * current stack home; reading its home as a memory divisor can alias
         * a reused slot and turn `i % 8` into `i % i`.  R10 is reserved as an
         * encoder scratch and is not an allocatable color.
         */
        if (!mach_load_op_rax(e, &in->src1) || !mach_mov_reg_rax(e, 10) ||
            !mach_load_op_rax(e, &in->src0) ||
            !mach_flush_pregs_to_home(e, (1u << 0) | (1u << 1) | (1u << 2) |
                                             (1u << 8) | (1u << 9) |
                                             (1u << 10)) ||
            !mach_rex(e, true, false, false, false) || !mach_u8(e, 0x99) ||
            !mach_rex(e, true, false, false, true) || !mach_u8(e, 0xf7) ||
            !mach_u8(e, 0xfa))
          return false;
        if (in->opcode == NYR_MACH_MOD) {
          /*
           * mov %rdx, %rax then dual-write commit
           */
          if (!mach_rex(e, true, false, false, false) || !mach_u8(e, 0x89) ||
              !mach_u8(e, 0xd0))
            return false;
          if (!mach_commit_vreg(e, &in->dst, dst))
            return false;
        } else if (!mach_commit_vreg(e, &in->dst, dst))
          return false;
        nyr_native_mach_div_record(false);
        break;
      case NYR_MACH_SELECT: {
        /* Seed the true arm before establishing the predicate flags.
         * Rematerializing a zero in its assigned register uses XOR, which
         * would otherwise overwrite the bit-test flags below. Operand live
         * ranges overlap at this instruction, so subsequent operand loads
         * cannot reuse this register for a different live value. */
        int true_preg = -1;
        if (in->src1.kind == NYR_MACH_OPERAND_VREG) {
          if (!mach_seed_colored_preg(e, in->src1.as.reg))
            return false;
          true_preg = mach_vreg_preg(e, in->src1.as.reg);
        } else {
          return mach_err(
              e,
              "x64 machine form encode: select true value is not a register");
        }
        /*
         * Keep the predicate across operand materialization.  Loading an
         * immediate zero may use xor, which changes flags; testing the
         * predicate before loading the false value made cmovne observe the
         * materializer's flags and select the wrong arm.  R10 is reserved
         * from allocator colors and is available as a short-lived scratch.
         */
        int32_t fused_test_src = in->src0.kind == NYR_MACH_OPERAND_VREG &&
                                         e->select_test_src &&
                                         in->src0.as.reg < e->colors_len
                                     ? e->select_test_src[in->src0.as.reg]
                                     : -1;
        if (fused_test_src >= 0) {
          /* Both arms are live at SELECT, so seed them before establishing
           * flags. Select directly into the allocated result register;
           * when it aliases the true arm, invert the condition instead of
           * destroying that arm with the initial false-value copy. */
          if (true_preg >= 0 && in->src2.kind == NYR_MACH_OPERAND_VREG &&
              in->dst.kind == NYR_MACH_OPERAND_VREG) {
            if (!mach_seed_colored_preg(e, in->src2.as.reg) ||
                !mach_seed_colored_preg(e, (uint32_t)fused_test_src))
              return false;
            int false_preg = mach_vreg_preg(e, in->src2.as.reg);
            int result_preg = mach_vreg_def_preg(e, in->dst.as.reg);
            int test_preg = mach_vreg_preg(e, (uint32_t)fused_test_src);
            if (false_preg >= 0 && result_preg >= 0 && test_preg >= 0) {
              bool equal = e->select_test_equal[in->src0.as.reg] != 0;
              int other_preg = true_preg;
              if (!mach_rex(e, true, false, false, test_preg >= 8) ||
                  !mach_u8(e, 0xf7) ||
                  !mach_u8(e, (unsigned char)(0xc0 | (test_preg & 7))) ||
                  !mach_i32(e, 1))
                return false;
              if (result_preg == true_preg) {
                equal = !equal;
                other_preg = false_preg;
              } else if (!mach_mov_preg_preg(e, result_preg, false_preg)) {
                return false;
              }
              if (!mach_rex(e, true, result_preg >= 8, false,
                            other_preg >= 8) ||
                  !mach_u8(e, 0x0f) || !mach_u8(e, equal ? 0x44 : 0x45) ||
                  !mach_u8(e, (unsigned char)(0xc0 | ((result_preg & 7) << 3) |
                                              (other_preg & 7))) ||
                  !mach_commit_preg(e, &in->dst, dst, (unsigned)result_preg,
                                    false))
                return false;
              break;
            }
          }
          if (!mach_load_op_preg(e, &in->src2, 10) ||
              !mach_seed_colored_preg(e, (uint32_t)fused_test_src))
            return false;
          int test_preg = mach_vreg_preg(e, (uint32_t)fused_test_src);
          if (test_preg == 0) {
            if (!mach_rex(e, true, false, false, false) || !mach_u8(e, 0xa9) ||
                !mach_i32(e, 1))
              return false;
          } else if (test_preg > 0) {
            /* Test the already-resident low bit without staging it in RAX. */
            if (!mach_rex(e, true, false, false, test_preg >= 8) ||
                !mach_u8(e, 0xf7) ||
                !mach_u8(e, (unsigned char)(0xc0 | (test_preg & 7))) ||
                !mach_i32(e, 1))
              return false;
          } else {
            if (!mach_load_op_rax(e,
                                  &(nyr_mach_operand_t){
                                      .kind = NYR_MACH_OPERAND_VREG,
                                      .reg_class = NYR_MACH_REGCLASS_GPR,
                                      .as.reg = (uint32_t)fused_test_src}) ||
                !mach_rex(e, true, false, false, false) || !mach_u8(e, 0xa9) ||
                !mach_i32(e, 1))
              return false;
          }
          if (!mach_rex(e, true, true, false, false) || !mach_u8(e, 0x89) ||
              !mach_u8(e, 0xd0))
            return false;
        } else if (!mach_load_op_rax(e, &in->src0) ||
                   !mach_mov_reg_rax(e, 10) || !mach_load_op_rax(e, &in->src2))
          return false;
        /*
         * test r10,r10 (REX.WRB + 85 /r), immediately before cmovne.
         */
        unsigned char cmov_opcode = 0x45; /* cmovne for a materialized truth */
        if (fused_test_src >= 0) {
          bool test_equal = e->select_test_equal[in->src0.as.reg] != 0;
          cmov_opcode = test_equal ? 0x44 : 0x45;
        } else if (!mach_rex(e, true, true, false, true) || !mach_u8(e, 0x85) ||
                   !mach_u8(e, 0xd2))
          return false;
        /*
         * REX.W + 0F 45 /r: cmovne true_value, false_value(%rax).
         */
        if (true_preg >= 0) {
          if (!mach_rex(e, true, false, false, true_preg >= 8) ||
              !mach_u8(e, 0x0f) || !mach_u8(e, cmov_opcode) ||
              !mach_u8(e, (unsigned char)(0xc0 | (true_preg & 7))))
            return false;
        } else {
          int true_slot = mach_slot_off(e, &in->src1);
          if (!mach_rex(e, true, false, false, false) || !mach_u8(e, 0x0f) ||
              !mach_u8(e, cmov_opcode) || !mach_u8(e, 0x85) ||
              !mach_i32(e, true_slot))
            return false;
        }
        if (!mach_commit_vreg(e, &in->dst, dst))
          return false;
        break;
      }
      case NYR_MACH_CMP: {
        uint8_t cmp_action = in->dst.kind == NYR_MACH_OPERAND_VREG &&
                                     e->cmp_elision_action &&
                                     in->dst.as.reg < e->colors_len
                                 ? e->cmp_elision_action[in->dst.as.reg]
                                 : 0;
        if (cmp_action == MACH_CMP_SKIP_FOR_SELECT)
          break;
        if (cmp_action == MACH_CMP_REUSE_FLAGS) {
          if (!mach_cmp_branch_fusable(e, blk, e->cur_inst, in, false, NULL,
                                       NULL, NULL, NULL))
            return mach_err(e,
                            "x64 machine form encode: unsafe reused compare");
          e->fused_jcc = mach_cond_jcc(in->condition, true);
          break;
        }
        /*
         * Flags-fused compare: if this compare's result is consumed only by
         * an immediately following BR_IF in the same block, keep RFLAGS from
         * the cmp and let the branch emit jcc directly.  setcc, movzbq, and
         * stores never modify flags, so they stay live through the old
         * materialization.  The cond vreg must die at the branch (segment
         * ends there) and must not be live-out, so nothing can later reseed
         * the stale home.
         */
        const char *fuse_reason = NULL;
        size_t fuse_barrier_idx = SIZE_MAX;
        nyr_mach_opcode_t fuse_barrier_opcode = NYR_MACH_NOP;
        bool fuse_branch = mach_cmp_branch_fusable(
            e, blk, e->cur_inst, in, true, NULL, &fuse_reason,
            &fuse_barrier_idx, &fuse_barrier_opcode);
        if (ny_dbg_verbosity() >= 6 && name && ny_dbg_function_selected(name)) {
          const char *filter = getenv("NY_TRACE_X64_CMP_FUSE_FILTER");
          if (filter && *filter && strstr(name, filter)) {
            char barrier[64];
            if (fuse_barrier_idx == SIZE_MAX)
              snprintf(barrier, sizeof(barrier), "-");
            else
              snprintf(barrier, sizeof(barrier), "%zu:%s", fuse_barrier_idx,
                       nyr_mach_opcode_name(fuse_barrier_opcode));
            ny_dbg_enter_context(name);
            ny_dbg_heuristic_event(
                "X64_CMP_BRANCH",
                "inst=%zu block=%zu vreg=%u fuse=%d reason=%s barrier=%s",
                e->cur_inst, e->cur_block, in->dst.as.reg, (int)fuse_branch,
                fuse_reason, barrier);
            ny_dbg_exit_context(name);
          }
        }
        if (mach_is_float(mach, &in->src0)) {
          bool f32 = mach_is_f32(mach, &in->src0);
          if (e->fp_fast_path) {
            unsigned lhs = 14, rhs = 15;
            if (!mach_load_float_operand(e, &in->src0, f32, 14, &lhs) ||
                !mach_load_float_operand(e, &in->src1, f32, 15, &rhs) ||
                !mach_sse_cmp_reg(e, lhs, rhs, f32))
              return false;
          } else {
            if (!mach_load_xmm0(e, a, f32) || !mach_load_xmm1(e, b, f32))
              return false;
            /*
             * ucomis[sd] xmm0, xmm1: f32=0F 2E C1, f64=66 0F 2E C1
             */
            if (f32) {
              if (!mach_u8(e, 0x0f) || !mach_u8(e, 0x2e) || !mach_u8(e, 0xc1))
                return false;
            } else if (!mach_u8(e, 0x66) || !mach_u8(e, 0x0f) ||
                       !mach_u8(e, 0x2e) || !mach_u8(e, 0xc1))
              return false;
          }
          if (fuse_branch) {
            e->fused_jcc = mach_cond_jcc(in->condition, true);
            break;
          }
          unsigned char setcc = mach_cond_setcc(in->condition, true);
          if (!setcc)
            return mach_err(e, "x64 machine form encode: bad float cond");
          if (!mach_u8(e, 0x0f) || !mach_u8(e, setcc) || !mach_u8(e, 0xc0) ||
              !mach_rex(e, true, false, false, false) || !mach_u8(e, 0x0f) ||
              !mach_u8(e, 0xb6) || !mach_u8(e, 0xc0) ||
              !mach_commit_vreg(e, &in->dst, dst))
            return false;
          break;
        }
        if (!mach_is_i64(mach, &in->src0))
          return mach_err(e, "x64 machine form encode: unsupported cmp type");
        bool direct_cmp = false;
        if (in->src0.kind == NYR_MACH_OPERAND_VREG) {
          int lhs = mach_vreg_preg(e, in->src0.as.reg);
          if (!direct_cmp && lhs >= 0) {
            int64_t imm = 0;
            int rhs = -1;
            if (mach_operand_const_i64(e, &in->src1, &imm) &&
                (int64_t)(int32_t)imm == imm) {
              if (imm == 0) {
                if (!mach_rex(e, true, lhs >= 8, false, lhs >= 8) ||
                    !mach_u8(e, 0x85) ||
                    !mach_u8(e, 0xc0 | ((lhs & 7) << 3) | (lhs & 7)))
                  return false;
              } else if (imm >= -128 && imm <= 127) {
                if (!mach_rex(e, true, false, false, lhs >= 8) ||
                    !mach_u8(e, 0x83) || !mach_u8(e, 0xf8 | (lhs & 7)) ||
                    !mach_u8(e, (unsigned char)imm))
                  return false;
              } else if (!mach_rex(e, true, false, false, lhs >= 8) ||
                         !mach_u8(e, 0x81) || !mach_u8(e, 0xf8 | (lhs & 7)) ||
                         !mach_i32(e, (int32_t)imm)) {
                return false;
              }
              direct_cmp = true;
            } else if (in->src1.kind == NYR_MACH_OPERAND_VREG &&
                       (rhs = mach_vreg_preg(e, in->src1.as.reg)) >= 0) {
              if (!mach_rex(e, true, lhs >= 8, false, rhs >= 8) ||
                  !mach_u8(e, 0x3b) ||
                  !mach_u8(e, 0xc0 | ((lhs & 7) << 3) | (rhs & 7)))
                return false;
              direct_cmp = true;
            }
          }
        }
        if (!direct_cmp && !mach_load_op_rax(e, &in->src0))
          return false;
        if (!direct_cmp) {
          /*
           * cmp rax, imm32 (3d, sign-extended) when src1 is a known constant.
           */
          int64_t fold = 0;
          bool fold_ok = false;
          if (in->src1.kind == NYR_MACH_OPERAND_IMM) {
            fold = in->src1.as.imm;
            fold_ok = (int64_t)(int32_t)fold == fold;
          } else if (in->src1.kind == NYR_MACH_OPERAND_VREG && imm_fold_known &&
                     in->src1.as.reg < mach->vreg_len &&
                     imm_fold_known[in->src1.as.reg]) {
            fold = imm_fold[in->src1.as.reg];
            fold_ok = (int64_t)(int32_t)fold == fold;
          }
          if (fold_ok) {
            if (fold == 0) {
              /*
               * test %rax,%rax is the shortest signed/zero compare against 0.
               */
              if (!mach_rex(e, true, false, false, false) ||
                  !mach_u8(e, 0x85) || !mach_u8(e, 0xc0))
                return false;
            } else if (!mach_rex(e, true, false, false, false) ||
                       !mach_u8(e, 0x3d) || !mach_i32(e, (int32_t)fold)) {
              return false;
            }
          } else {
            int rhs_preg = -1;
            if (in->src1.kind == NYR_MACH_OPERAND_VREG) {
              if (!mach_seed_colored_preg(e, in->src1.as.reg))
                return false;
              rhs_preg = mach_vreg_preg(e, in->src1.as.reg);
            }
            if (rhs_preg >= 0) {
              if (!mach_rex(e, true, false, false, rhs_preg >= 8) ||
                  !mach_u8(e, 0x3b) ||
                  !mach_u8(e, (unsigned char)(0xc0 | (rhs_preg & 7))))
                return false;
            } else if (!mach_rex(e, true, false, false, false) ||
                       !mach_u8(e, 0x3b) || !mach_u8(e, 0x85) ||
                       !mach_i32(e, b))
              return false;
          }
        }
        if (fuse_branch) {
          e->fused_jcc = mach_cond_jcc(in->condition, false);
          break;
        }
        unsigned char setcc = mach_cond_setcc(in->condition, false);
        if (!setcc)
          return mach_err(e, "x64 machine form encode: bad cmp cond");
        if (!mach_u8(e, 0x0f) || !mach_u8(e, setcc) || !mach_u8(e, 0xc0) ||
            !mach_rex(e, true, false, false, false) || !mach_u8(e, 0x0f) ||
            !mach_u8(e, 0xb6) || !mach_u8(e, 0xc0) ||
            !mach_commit_vreg(e, &in->dst, dst))
          return false;
        break;
      }
      case NYR_MACH_BR:
        /*
         * A predecessor may take either edge. XMM-resident scalar and vector
         * values must be homed before the branch; post-branch saves only
         * protect fall-through execution.
         */
        if (!mach_flush_float_to_home(e) || !mach_flush_vector_to_home(e))
          return false;
        /*
         * jmp rel32
         */
        mach_rax_invalidate(e);
        /*
         * Blocks are emitted in machine-block order.  An unconditional edge
         * to the immediately following block is already satisfied by normal
         * instruction fall-through; emitting jmp rel32 here only adds a
         * dynamic branch to the hot path.
         */
        if (e->cur_block_valid && in->src1.as.block_index == e->cur_block + 1)
          break;
        if (!mach_u8(e, 0xe9) || !mach_add_patch(e, in->src1.as.block_index) ||
            !mach_i32(e, 0))
          return false;
        break;
      case NYR_MACH_BR_IF:
        /*
         * XMM/vector values are caller-saved and must be materialized before
         * either branch edge. A resident GPR condition can be tested
         * directly; other conditions use their canonical home.
         */
        if (!mach_flush_float_to_home(e) || !mach_flush_vector_to_home(e))
          return false;
        mach_rax_invalidate(e);
        if (e->fused_jcc) {
          /*
           * Flags-fused: the adjacent CMP left RFLAGS live, so branch on the
           * compare condition directly instead of reloading the materialized
           * condition value from its home.
           */
          unsigned char jcc = e->fused_jcc;
          e->fused_jcc = 0;
          if (!mach_u8(e, 0x0f) || !mach_u8(e, jcc) ||
              !mach_add_patch(e, in->src1.as.block_index) || !mach_i32(e, 0))
            return false;
          break;
        }
        int condition_reg = -1;
        if (in->src0.kind == NYR_MACH_OPERAND_VREG) {
          /*
           * A branch may be the first use of a newly split colored segment.
           * Seed it from its canonical home before testing the preg;
           * otherwise the register can still contain a value from the
           * previous segment.
           */
          if (!mach_seed_colored_preg(e, in->src0.as.reg))
            return false;
          condition_reg = mach_vreg_preg(e, in->src0.as.reg);
        }
        if (condition_reg >= 0) {
          /*
           * testq %reg, %reg; jne target
           */
          unsigned reg = (unsigned)condition_reg;
          if (!mach_rex(e, true, reg >= 8, false, reg >= 8) ||
              !mach_u8(e, 0x85) ||
              !mach_u8(e, 0xc0 | ((reg & 7) << 3) | (reg & 7)) ||
              !mach_u8(e, 0x0f) || !mach_u8(e, 0x85) ||
              !mach_add_patch(e, in->src1.as.block_index) || !mach_i32(e, 0))
            return false;
          break;
        }
        /*
         * cmpq $0, mem; jne rel32
         */
        if (!mach_rex(e, true, false, false, false) || !mach_u8(e, 0x83) ||
            !mach_u8(e, 0xbd) || !mach_i32(e, a) || !mach_u8(e, 0x00) ||
            !mach_u8(e, 0x0f) || !mach_u8(e, 0x85) ||
            !mach_add_patch(e, in->src1.as.block_index) || !mach_i32(e, 0))
          return false;
        break;
      case NYR_MACH_CALL: {
        if (in->src0.kind != NYR_MACH_OPERAND_SYMBOL || !in->src0.as.symbol)
          return mach_err(e, "x64 machine form encode: call needs symbol");
        /* Publish ordinary colored values before the ABI call. Dedicated
         * loop values use their explicit canonical-home bridge. */
        if (!mach_loop_gprs_call_home(e, false) ||
            !mach_flush_colored_to_home(e) || !mach_flush_float_to_home(e) ||
            !mach_flush_vector_to_home(e))
          return false;
        /*
         * SysV: up to 6 GP + 8 SSE; aggregates ≤16B split per arg_sizes
         * class.
         */
        bool is_win = e->target && e->target->abi == NY_NATIVE_ABI_WIN64;
        static const int gp_sysv[] = {7, 6, 2,
                                      1, 8, 9}; /* rdi rsi rdx rcx r8 r9 */
        static const int gp_win[] = {1, 2, 8, 9};
        const int *gp = is_win ? gp_win : gp_sysv;
        size_t max_gp = is_win ? 4 : 6;
        size_t max_fp = is_win ? 4 : 8;
        size_t gp_n = 0, fp_n = 0, ordinal_n = 0, stack_n = 0;
        bool gp_arg[NYIR_CALL_MAX_ARGS] = {0};
        for (size_t ai = 0; ai < NYIR_CALL_MAX_ARGS; ++ai)
          gp_arg[ai] = false;
        bool agg_reg[NYIR_CALL_MAX_ARGS] = {0};
        bool agg_mem[NYIR_CALL_MAX_ARGS] = {0}; /* by-value on stack */
        uint32_t agg_size[NYIR_CALL_MAX_ARGS] = {0};
        if (in->args_len > NYIR_CALL_MAX_ARGS)
          return mach_err(e, "x64 machine form encode: too many call args");
        for (size_t ai = 0; ai < in->args_len; ++ai) {
          if (in->arg_sizes && in->arg_sizes[ai] > 0) {
            uint32_t size = NYIR_ARG_AGG_SIZE(in->arg_sizes[ai]);
            unsigned c0 = NYIR_ARG_AGG_CLASS(in->arg_sizes[ai], 0);
            unsigned c1 = NYIR_ARG_AGG_CLASS(in->arg_sizes[ai], 1);
            unsigned gpn = 0, fpn = 0;
            bool ok = size > 0 && size <= 16;
            if (c0 == NYIR_ARG_CLASS_INTEGER)
              gpn++;
            else if (c0 == NYIR_ARG_CLASS_SSE)
              fpn++;
            else if (c0 != NYIR_ARG_CLASS_NONE)
              ok = false;
            if (size > 8) {
              if (c1 == NYIR_ARG_CLASS_INTEGER)
                gpn++;
              else if (c1 == NYIR_ARG_CLASS_SSE)
                fpn++;
              else if (c1 != NYIR_ARG_CLASS_NONE)
                ok = false;
            }
            agg_size[ai] = size;
            if (ok && !is_win && gp_n + gpn <= max_gp && fp_n + fpn <= max_fp) {
              agg_reg[ai] = true;
              gp_n += gpn;
              fp_n += fpn;
            } else {
              /*
               * MEMORY class / oversized: copy aggregate onto the stack.
               */
              agg_mem[ai] = true;
              stack_n += (size + 7) / 8;
            }
            continue;
          }
          bool is_fp = mach_is_float(mach, &in->args[ai]);
          if (is_win) {
            if (ordinal_n >= max_gp)
              ++stack_n;
            if (!is_fp) {
              gp_arg[ai] = true;
            }
            ++ordinal_n;
          } else if (is_fp) {
            if (fp_n < max_fp)
              ++fp_n;
            else
              ++stack_n;
          } else if (gp_n < max_gp) {
            gp_arg[ai] = true;
            ++gp_n;
          } else {
            gp_arg[ai] = true;
            ++stack_n;
          }
        }
        if (!mach_flush_float_to_home(e) || !mach_flush_vector_to_home(e))
          return false;
        /*
         * Register-classified integer arguments can move directly into their
         * ABI register when the source is a non-aliasing callee-saved color,
         * an immediate/frame operand, or an uncolored value with a canonical
         * home. RSP staging is limited to transient RAX-forwarded values and
         * caller-saved source colors. Compact spill indices below ensure
         * direct arguments reserve no staging slots.
         */
        bool gp_direct[NYIR_CALL_MAX_ARGS] = {0};
        for (size_t ai = 0; ai < in->args_len; ++ai) {
          if (!gp_arg[ai])
            continue;
          const nyr_mach_operand_t *arg = &in->args[ai];
          bool safe;
          switch (arg->kind) {
          case NYR_MACH_OPERAND_IMM:
          case NYR_MACH_OPERAND_FRAME:
            safe = true;
            break;
          case NYR_MACH_OPERAND_VREG: {
            /*
             * A vreg colored in a caller-saved preg is a legal source only
             * through the staging slot: its segment ends at this call, so
             * an earlier argument move into the same ABI register would
             * clobber it. Values forwarded transiently through %rax
             * (sticky/pending tags) must also keep the staged path because
             * the staging loop's %rax traffic would invalidate the tag
             * before the direct placement runs.
             */
            int sp = mach_vreg_preg(e, arg->as.reg);
            int off = mach_vreg_home_off(e, arg->as.reg);
            bool sticky_rax =
                (!e->no_sticky && e->last_rax_valid &&
                 e->last_rax_off == off) ||
                (e->pending_rax_valid && e->pending_rax_off == off);
            /*
             * A resident callee-saved color is independent of RAX's sticky
             * stack-home cache. Its valid preg was captured before the call
             * flush above. Only uncolored values need the cache check here.
             */
            bool callee_saved = sp == 3 || (sp >= 12 && sp <= 15);
            safe = callee_saved || (sp < 0 && !sticky_rax);
            break;
          }
          default:
            safe = false;
            break;
          }
          gp_direct[ai] = safe;
        }
        int gp_spill_index[NYIR_CALL_MAX_ARGS];
        for (size_t ai = 0; ai < NYIR_CALL_MAX_ARGS; ++ai)
          gp_spill_index[ai] = -1;
        size_t gp_spill_n = 0;
        for (size_t ai = 0; ai < in->args_len; ++ai)
          if (gp_arg[ai] && !gp_direct[ai])
            gp_spill_index[ai] = (int)gp_spill_n++;
        size_t shadow = is_win && e->target ? e->target->shadow_space_bytes : 0;
        size_t outgoing_bytes = stack_n * 8 + shadow;
        size_t staging_bytes = gp_spill_n * 8;
        size_t stack_bytes =
            ((outgoing_bytes + staging_bytes) + 15) & ~(size_t)15;
        if (stack_bytes) {
          if (!mach_rex(e, true, false, false, false) || !mach_u8(e, 0x81) ||
              !mach_u8(e, 0xec) || !mach_i32(e, (int32_t)stack_bytes))
            return false;
        }
        /*
         * Stage only the shapes that cannot be placed directly. Direct
         * sources (immediates, homes, callee-saved colors) are written by the
         * placement pass below without an intermediate slot.
         */
        for (size_t ai = 0; ai < in->args_len; ++ai) {
          if (gp_spill_index[ai] < 0)
            continue;
          int stage_off =
              (int)(outgoing_bytes + (size_t)gp_spill_index[ai] * 8);
          if (!mach_load_call_arg_rax(e, &in->args[ai]) ||
              !mach_store_rax_rsp(e, stage_off))
            return false;
        }
        gp_n = fp_n = ordinal_n = 0;
        size_t stack_i = 0;
        /*
         * Two-pass placement: MEMORY-class by-value aggregates are memcpy'd
         * FIRST because the copy uses rsi/rdi/rcx, which would clobber any
         * register arguments placed before it.
         */
        for (size_t ai = 0; ai < in->args_len; ++ai) {
          if (!(in->arg_sizes && in->arg_sizes[ai] > 0 && agg_mem[ai]))
            continue;
          uint32_t size = agg_size[ai];
          if (!mach_load_op_rax(e, &in->args[ai]) ||
              !mach_rex(e, true, false, false, false) || !mach_u8(e, 0x89) ||
              !mach_u8(e, 0xc6) || /* mov %rax,%rsi */
              !mach_rex(e, true, false, false, false) || !mach_u8(e, 0x8d) ||
              !mach_u8(e, 0xbc) || !mach_u8(e, 0x24) ||
              !mach_i32(e, (int32_t)(shadow +
                                     stack_i * 8)) || /* lea disp(%rsp),%rdi */
              !mach_rex(e, true, false, false, false) ||
              !mach_u8(e, 0xc7) || !mach_u8(e, 0xc1) ||
              !mach_i32(e, (int32_t)size) || /* mov $size,%rcx */
              !mach_u8(e, 0xf3) || !mach_u8(e, 0xa4))
            return false;
        }
        for (size_t ai = 0; ai < in->args_len; ++ai) {
          int aoff = mach_slot_off(e, &in->args[ai]);
          if (in->arg_sizes && in->arg_sizes[ai] > 0 && agg_mem[ai]) {
            /*
             * already copied above; still advance stack slot order
             */
            size_t size8 = (agg_size[ai] + 7) / 8;
            bool is_win_arg = is_win;
            (void)is_win_arg;
            if (stack_i + size8 <= stack_n)
              stack_i += size8;
            continue;
          }
          if (in->arg_sizes && in->arg_sizes[ai] > 0) {
            if (agg_reg[ai]) {
              if (!mach_load_op_rax(e, &in->args[ai]))
                return false;
              unsigned c0 = NYIR_ARG_AGG_CLASS(in->arg_sizes[ai], 0);
              unsigned c1 = NYIR_ARG_AGG_CLASS(in->arg_sizes[ai], 1);
              uint32_t size = agg_size[ai];
              unsigned classes[2] = {c0, size > 8 ? c1 : NYIR_ARG_CLASS_NONE};
              for (int ch = 0; ch < 2; ++ch) {
                if (classes[ch] == NYIR_ARG_CLASS_NONE)
                  continue;
                if (classes[ch] == NYIR_ARG_CLASS_INTEGER) {
                  int reg = gp[gp_n++];
                  unsigned char modrm =
                      (unsigned char)(0x40 | ((reg & 7) << 3));
                  if (!mach_rex(e, true, reg >= 8, false, false) ||
                      !mach_u8(e, 0x8b) || !mach_u8(e, modrm) ||
                      !mach_u8(e, (unsigned)(ch * 8)))
                    return false;
                } else if (classes[ch] == NYIR_ARG_CLASS_SSE) {
                  unsigned xn = (unsigned)fp_n++;
                  unsigned char modrm = (unsigned char)(0x40 | ((xn & 7) << 3));
                  if (!mach_u8(e, 0xf2) || !mach_u8(e, 0x0f) ||
                      !mach_u8(e, 0x10) || !mach_u8(e, modrm) ||
                      !mach_u8(e, (unsigned)(ch * 8)))
                    return false;
                }
              }
            } else if (agg_mem[ai]) {
              /*
               * MEMORY by-value: memcpy aggregate onto stack arg area.
               */
              uint32_t size = agg_size[ai];
              size_t slots = (size + 7) / 8;
              if (!mach_load_op_rax(e, &in->args[ai]) ||
                  !mach_rex(e, true, false, false, false) ||
                  !mach_u8(e, 0x89) || !mach_u8(e, 0xc6) || /* mov %rax,%rsi */
                  !mach_rex(e, true, false, false, false) ||
                  !mach_u8(e, 0x8d) || !mach_u8(e, 0xbc) || !mach_u8(e, 0x24) ||
                  !mach_i32(e,
                            (int32_t)(shadow +
                                      stack_i * 8)) || /* lea disp(%rsp),%rdi */
                  !mach_rex(e, true, false, false, false) ||
                  !mach_u8(e, 0xc7) || !mach_u8(e, 0xc1) ||
                  !mach_i32(e, (int32_t)size) || /* mov $size,%rcx */
                  !mach_u8(e, 0xf3) || !mach_u8(e, 0xa4))
                return false;
              stack_i += slots;
              mach_rax_invalidate(e);
            }
            continue;
          }
          bool is_f32 = mach_is_f32(mach, &in->args[ai]);
          bool is_f64 = mach_is_f64(mach, &in->args[ai]);
          bool is_fp = is_f32 || is_f64;
          bool on_stack = is_win  ? ordinal_n >= max_gp
                          : is_fp ? fp_n >= max_fp
                                  : gp_n >= max_gp;
          const size_t location_index = is_win  ? ordinal_n
                                        : is_fp ? fp_n
                                                : gp_n;
          const size_t location_stack_index = stack_i;
          if (on_stack) {
            int stack_off = (int)(shadow + stack_i * 8);
            if (is_f32) {
              if (!mach_load_eax(e, aoff) || !mach_store_eax_rsp(e, stack_off))
                return false;
            } else if (gp_spill_index[ai] >= 0) {
              int stage_off =
                  (int)(outgoing_bytes + (size_t)gp_spill_index[ai] * 8);
              if (!mach_load_rax_rsp(e, stage_off) ||
                  !mach_store_rax_rsp(e, stack_off))
                return false;
            } else if (!mach_load_op_rax(e, &in->args[ai]) ||
                       !mach_rex(e, true, false, false, false) ||
                       !mach_u8(e, 0x89) || !mach_u8(e, 0x84) ||
                       !mach_u8(e, 0x24) || !mach_i32(e, stack_off))
              return false;
            ++stack_i;
          } else if (is_fp) {
            size_t index = is_win ? ordinal_n : fp_n;
            if (!mach_load_xmm(e, aoff, is_f32, (unsigned)index))
              return false;
            if (!is_win)
              ++fp_n;
          } else {
            size_t index = is_win ? ordinal_n : gp_n;
            int reg = gp[index];
            if (gp_spill_index[ai] >= 0) {
              if (!mach_load_rax_rsp(
                      e,
                      (int)(outgoing_bytes + (size_t)gp_spill_index[ai] * 8)) ||
                  !mach_mov_reg_rax(e, reg))
                return false;
            } else if (!mach_load_call_arg_preg(e, &in->args[ai],
                                                (unsigned)reg)) {
              return false;
            }
            if (!is_win)
              ++gp_n;
          }
          if (ny_dbg_enabled & NY_DBG_ABI) {
            /*
             * Call-site ABI placement (-vvvv ABI channel): the physical
             * location each argument occupies at the call instruction.
             */
            char place[48];
            if (on_stack)
              snprintf(place, sizeof(place), "stack+%d",
                       (int)(shadow + location_stack_index * 8));
            else if (is_fp)
              snprintf(place, sizeof(place), "xmm%zu", location_index);
            else {
              int preg = gp[location_index];
              static const char *const gp_names[16] = {
                  "%rax", "%rcx", "%rdx", "%rbx", "%rsp", "%rbp",
                  "%rsi", "%rdi", "%r8",  "%r9",  "%r10", "%r11",
                  "%r12", "%r13", "%r14", "%r15"};
              snprintf(place, sizeof(place), "%s", gp_names[preg & 15]);
            }
            char detail[128];
            snprintf(detail, sizeof(detail), "%s arg %zu",
                     in->src0.as.symbol ? in->src0.as.symbol : "<indirect>",
                     ai);
            ny_dbg_abi_classify("call-arg", place, detail);
          }
          if (is_win)
            ++ordinal_n;
        }
        char call_sym[256];
        const char *raw = in->src0.as.symbol;
        const char *pref = e->target && e->target->symbol_prefix
                               ? e->target->symbol_prefix
                               : "";
        if (in->call_is_extern || ny_native_runtime_abi_decl(raw) != NULL ||
            strncmp(raw, "ny_fn_", 6) == 0)
          snprintf(call_sym, sizeof(call_sym), "%s%s", pref, raw);
        else
          snprintf(call_sym, sizeof(call_sym), NY_FMT_FN, pref, raw);
        if (!mach_u8(e, 0xe8) || !mach_add_reloc(e, call_sym, NY_RELOC_PLT32) ||
            !mach_i32(e, 0))
          return false;
        if (stack_bytes) {
          if (!mach_rex(e, true, false, false, false) || !mach_u8(e, 0x81) ||
              !mach_u8(e, 0xc4) || !mach_i32(e, (int32_t)stack_bytes))
            return false;
        }
        /* Argument placement can re-seed a caller-saved color after the
         * pre-call flush. Invalidate those colors, then restore dedicated
         * loop values from their canonical homes. */
        mach_invalidate_call_clobbered_colors(e);
        if (!mach_loop_gprs_call_home(e, true))
          return false;
        mach_rax_invalidate(e);
        if (in->dst.kind == NYR_MACH_OPERAND_VREG) {
          if (mach_is_float(mach, &in->dst)) {
            bool f32 = mach_is_f32(mach, &in->dst);
            int preg =
                e->fp_fast_path ? mach_vreg_fpreg(e, in->dst.as.reg) : -1;
            if (preg >= 0) {
              if ((unsigned)preg != 0 &&
                  !mach_sse_mov_reg(e, (unsigned)preg, 0, f32))
                return false;
              if (e->fp_color_seeded)
                e->fp_color_seeded[in->dst.as.reg] = true;
            } else if (!mach_store_xmm0(e, mach_slot_off(e, &in->dst), f32))
              return false;
          } else if (!mach_commit_vreg(e, &in->dst, mach_slot_off(e, &in->dst)))
            return false;
        }
        /*
         * Callee-saved colors survive calls; sticky rax does not.
         */
        break;
      }
      case NYR_MACH_RET:
        if (in->src0.kind != NYR_MACH_OPERAND_NONE) {
          if (mach_is_float(mach, &in->src0)) {
            bool f32 = mach_is_f32(mach, &in->src0);
            int preg = in->src0.kind == NYR_MACH_OPERAND_VREG
                           ? mach_vreg_fpreg(e, in->src0.as.reg)
                           : -1;
            if (e->fp_fast_path && preg >= 0) {
              if (!mach_seed_float_preg(e, in->src0.as.reg) ||
                  (preg != 0 && !mach_sse_mov_reg(e, 0, (unsigned)preg, f32)))
                return false;
            } else if (!mach_load_xmm0(e, mach_slot_off(e, &in->src0), f32))
              return false;
          } else {
            /*
             * A return must materialize the value named by NYIR.
             */
            mach_rax_invalidate(e);
            if (!mach_load_op_rax(e, &in->src0))
              return false;
          }
        }
        /*
         * Epilogue: add frame; pop callee-saves; pop rbp; ret.
         * Do not use leave after pops — leave would reset %rsp to %rbp and
         * undo the callee-save pops.
         */
        if (frame) {
          if (!mach_rex(e, true, false, false, false) || !mach_u8(e, 0x81) ||
              !mach_u8(e, 0xc4) || !mach_i32(e, frame))
            return false;
        }
        if (!mach_pop_callee_saves(e, save_mask))
          return false;
        if (!mach_u8(e, 0x5d) || !mach_u8(e, 0xc3)) /* pop %rbp; ret */
          return false;
        break;
      case NYR_MACH_NOP:
        break;
      case NYR_MACH_TRAP:
        /*
         * ud2 — raises #UD (invalid opcode), terminating the process.
         */
        if (!mach_u8(e, 0x0f) || !mach_u8(e, 0x0b))
          return false;
        break;
      case NYR_MACH_INLINE_ASM:
        /*
         * Encode only templates with a defined x86-64 lowering.  An
         * unrecognised template is an owning encoder error, not a nop:
         * silently discarding inline assembly changes program semantics.
         */
        if (in->src0.kind != NYR_MACH_OPERAND_SYMBOL || !in->src0.as.symbol)
          return mach_err(e,
                          "x64 machine form encode: inline asm needs template");
        {
          const char *t = in->src0.as.symbol;
          if (strcmp(t, "pause") == 0 || strcmp(t, "pause;") == 0) {
            if (!mach_u8(e, 0xf3) || !mach_u8(e, 0x90))
              return false;
          } else if (strcmp(t, "lfence") == 0) {
            if (!mach_u8(e, 0x0f) || !mach_u8(e, 0xae) || !mach_u8(e, 0xe8))
              return false;
          } else if (strcmp(t, "mfence") == 0) {
            if (!mach_u8(e, 0x0f) || !mach_u8(e, 0xae) || !mach_u8(e, 0xf0))
              return false;
          } else if (strcmp(t, "sfence") == 0) {
            if (!mach_u8(e, 0x0f) || !mach_u8(e, 0xae) || !mach_u8(e, 0xf8))
              return false;
          } else if (strcmp(t, "ud2") == 0) {
            if (!mach_u8(e, 0x0f) || !mach_u8(e, 0x0b))
              return false;
          } else if (strncmp(t, "nop", 3) == 0) {
            /*
             * nop, nop;nop, multi-nop -> one or more 0x90.
             */
            bool emitted = false;
            for (const char *p = t; *p; ++p) {
              if ((p == t || p[-1] == ';') && (p[0] == 'n' || p[0] == 'N')) {
                if (!mach_u8(e, 0x90))
                  return false;
                emitted = true;
              }
            }
            if (!emitted)
              return mach_err(
                  e,
                  "x64 machine form encode: unsupported inline asm template");
          } else {
            return mach_err(
                e, "x64 machine form encode: unsupported inline asm template");
          }
        }
        break;
      case NYR_MACH_REDUCE_ADD:
        if (mach_is_f64(mach, &in->dst)) {
          if (!mach_reduce_add_f64(e, in, dst))
            return false;
        } else if (!mach_reduce_add_i64(e, in, dst)) {
          return false;
        }
        break;
      case NYR_MACH_INTRINSIC: {
        /*
         * COPY_STRUCT: src0=dst-ptr, src1=src-ptr, src2=IMM size.
         * CAPTURE_RET: dst=vreg, src0=IMM selector
         * (0=rdx,1=rax,2=xmm0,3=xmm1).
         */
        if (in->src2.kind == NYR_MACH_OPERAND_IMM &&
            in->src0.kind == NYR_MACH_OPERAND_VREG) {
          int64_t nbytes = in->src2.as.imm;
          if (nbytes <= 0)
            break;
          /*
           * rep movsb clobbers rsi/rdi/rcx; flush any colored vregs that
           * might be resident there before the copy (same as CALL/DIV).
           */
          if (!mach_flush_colored_to_home(e))
            return false;
          int dstp = mach_slot_off(e, &in->src0);
          int srcp = mach_slot_off(e, &in->src1);
          if (!mach_load_rax(e, srcp) ||
              !mach_rex(e, true, false, false, false) || !mach_u8(e, 0x89) ||
              !mach_u8(e, 0xc6) || !mach_load_rax(e, dstp) ||
              !mach_rex(e, true, false, false, false) || !mach_u8(e, 0x89) ||
              !mach_u8(e, 0xc7) || !mach_rex(e, true, false, false, false) ||
              !mach_u8(e, 0xc7) || !mach_u8(e, 0xc1) ||
              !mach_i32(e, (int32_t)nbytes) || !mach_u8(e, 0xf3) ||
              !mach_u8(e, 0xa4))
            return false;
          mach_rax_invalidate(e);
          break;
        }
        if (in->src0.kind != NYR_MACH_OPERAND_IMM)
          return mach_err(e, "x64 machine form encode: bad intrinsic shape");
        unsigned sel = (unsigned)(in->src0.as.imm & 0xff);
        if (sel > 3)
          return mach_err(e,
                          "x64 machine form encode: bad capture.ret selector");
        if (sel == 0) {
          if (!mach_rex(e, true, false, false, false) || !mach_u8(e, 0x89) ||
              !mach_u8(e, 0xd0))
            return false;
        } else if (sel == 2) {
          if (!mach_u8(e, 0x66) || !mach_rex(e, true, false, false, false) ||
              !mach_u8(e, 0x0f) || !mach_u8(e, 0x7e) || !mach_u8(e, 0xc0))
            return false;
        } else if (sel == 3) {
          if (!mach_u8(e, 0x66) || !mach_rex(e, true, false, false, false) ||
              !mach_u8(e, 0x0f) || !mach_u8(e, 0x7e) || !mach_u8(e, 0xc8))
            return false;
        }
        /*
         * sel==1: rax already primary return
         */
        mach_rax_invalidate(e);
        if (in->dst.kind == NYR_MACH_OPERAND_VREG) {
          if (!mach_commit_vreg(e, &in->dst, mach_slot_off(e, &in->dst)))
            return false;
        }
        break;
      }
      default:
        return mach_err(
            e, "x64 machine form encode: unsupported opcode (NYIR fallback)");
      }
      if (in->dst.kind == NYR_MACH_OPERAND_VREG && e->color_seeded &&
          in->dst.as.reg < e->colors_len &&
          mach_vreg_preg(e, in->dst.as.reg) >= 0)
        e->color_seeded[in->dst.as.reg] = true;
      if (!mach_end_segments(e))
        return false;
      /*
       * Legacy conversion and square-root sequences may use low XMM
       * scratch registers. Dedicated values have current canonical homes.
       */
      if (e->loop_fpr &&
          (in->opcode == NYR_MACH_CONVERT || in->opcode == NYR_MACH_SQRT))
        for (uint32_t v = 0; v < e->fp_colors_len; ++v)
          if (e->loop_fpr[v] >= 0)
            e->fp_color_seeded[v] = false;
      if (e->loop_fpr && in->dst.kind == NYR_MACH_OPERAND_VREG &&
          in->opcode != NYR_MACH_STORE && e->loop_fpr[in->dst.as.reg] >= 0) {
        uint32_t v = in->dst.as.reg;
        if (!mach_store_xmm(e, mach_vreg_home_off(e, v),
                            mach->vreg_types[v] == NYR_MACH_TYPE_F32,
                            (unsigned)e->loop_fpr[v]))
          return false;
        for (uint32_t other = 0; other < e->fp_colors_len; ++other)
          if (e->loop_fpr[other] == e->loop_fpr[v])
            e->fp_color_seeded[other] = false;
        e->fp_color_seeded[v] = true;
      }
      if (!mach_float_end_segments(e))
        return false;
      if (!mach_vector_end_segments(e))
        return false;
      if (e->pending_rax_valid) {
        e->last_rax_valid = true;
        e->last_rax_off = e->pending_rax_off;
      }
    }
    if (!mach_save_float_live_out(e, bi))
      return false;
    if (!mach_save_vector_live_out(e, bi))
      return false;
  }

  size_t colored_segments = 0;
  size_t spilled_segments = 0;
  size_t reload_segments = 0;
  for (size_t i = 0; i < e->regalloc.segment_len; ++i) {
    if (e->regalloc.segments[i].color >= 0)
      ++colored_segments;
    else
      ++spilled_segments;
    if (e->regalloc.segments[i].reload)
      ++reload_segments;
  }
  size_t gpr_peak_live =
      nyr_mach_regalloc_peak_live(&e->regalloc, e->mach->inst_len);
  nyr_native_mach_regalloc_record(e->regalloc.segment_len, colored_segments,
                                  spilled_segments, reload_segments,
                                  gpr_peak_live);
  size_t fp_colored_segments = 0;
  size_t fp_spilled_segments = 0;
  size_t fp_reload_segments = 0;
  for (size_t i = 0; i < e->fp_regalloc.segment_len; ++i) {
    if (e->fp_regalloc.segments[i].color >= 0)
      ++fp_colored_segments;
    else
      ++fp_spilled_segments;
    if (e->fp_regalloc.segments[i].reload)
      ++fp_reload_segments;
  }
  size_t fp_peak_live =
      nyr_mach_regalloc_peak_live(&e->fp_regalloc, e->mach->inst_len);
  nyr_native_mach_fpr_record(e->fp_regalloc.segment_len, fp_colored_segments,
                             fp_spilled_segments, fp_reload_segments,
                             fp_peak_live);
  size_t vec_colored_segments = 0;
  size_t vec_spilled_segments = 0;
  size_t vec_reload_segments = 0;
  for (size_t i = 0; i < e->vec_regalloc.segment_len; ++i) {
    if (e->vec_regalloc.segments[i].color >= 0)
      ++vec_colored_segments;
    else
      ++vec_spilled_segments;
    if (e->vec_regalloc.segments[i].reload)
      ++vec_reload_segments;
  }
  size_t vec_peak_live =
      nyr_mach_regalloc_peak_live(&e->vec_regalloc, e->mach->inst_len);
  nyr_native_mach_vector_record(e->vec_regalloc.segment_len,
                                vec_colored_segments, vec_spilled_segments,
                                vec_reload_segments, vec_peak_live);
  nyr_mach_regalloc_free(&e->vec_regalloc);
  nyr_mach_regalloc_free(&e->regalloc);
  nyr_mach_regalloc_free(&e->fp_regalloc);
  if (e->color_seeded && e->color_seeded != color_seeded_stack)
    free(e->color_seeded);
  e->color_seeded = NULL;
  if (e->fp_color_seeded && e->fp_color_seeded != fp_color_seeded_stack)
    free(e->fp_color_seeded);
  e->fp_color_seeded = NULL;
  if (e->vec_color_seeded && e->vec_color_seeded != vec_color_seeded_stack)
    free(e->vec_color_seeded);
  e->vec_color_seeded = NULL;
  mach_free_remat_maps(e);
  free(e->def_inst);
  e->def_inst = NULL;
  /*
   * Patch relative jumps
   */
  for (size_t i = 0; i < e->patch_count; ++i) {
    size_t at = e->patch_at[i];
    uint32_t blk = e->patch_block[i];
    if (blk >= mach->block_len)
      return mach_err(e, "x64 machine form encode: bad branch target");
    int32_t rel = (int32_t)((int64_t)e->block_off[blk] - (int64_t)(at + 4));
    e->code.data[at] = (unsigned char)(rel & 0xff);
    e->code.data[at + 1] = (unsigned char)((rel >> 8) & 0xff);
    e->code.data[at + 2] = (unsigned char)((rel >> 16) & 0xff);
    e->code.data[at + 3] = (unsigned char)((rel >> 24) & 0xff);
  }
  return true;
}

typedef struct {
  nyr_obj_buf_t code;
  nyr_x64_obj_reloc_t *relocs;
  size_t reloc_count;
  char symbol[256];
  char error[256];
} ny_x64_parallel_encode_result_t;

typedef struct {
  const nyr_mach_func_t *funcs;
  const char *const *names;
  const size_t *order;
  const ny_native_target_info_t *target;
  ny_x64_parallel_encode_result_t *results;
} ny_x64_parallel_encode_ctx_t;

static bool ny_x64_parallel_encode_task(size_t oi, void *opaque) {
  ny_x64_parallel_encode_ctx_t *ctx = (ny_x64_parallel_encode_ctx_t *)opaque;
  size_t i = ctx->order ? ctx->order[oi] : oi;
  ny_x64_parallel_encode_result_t *r = &ctx->results[oi];
  const char *nm = ctx->names && ctx->names[i] ? ctx->names[i] : "fn";
  snprintf(r->symbol, sizeof(r->symbol), NY_FMT_FN,
           ctx->target->symbol_prefix ? ctx->target->symbol_prefix : "", nm);
  nyr_mach_func_t mach_mut = ctx->funcs[i];
  (void)nyr_isle_apply_mach(&mach_mut);
  nyr_x64_mach_enc_t enc = {0};
  enc.mach = &mach_mut;
  enc.target = ctx->target;
  enc.err = r->error;
  enc.err_len = sizeof(r->error);
  bool ok = mach_encode_function(&enc, r->symbol, false);
  mach_free_remat_maps(&enc);
  free(enc.block_off);
  free(enc.patch_at);
  free(enc.patch_block);
  if (!ok) {
    free(enc.code.data);
    free(enc.relocs);
    return false;
  }
  r->code = enc.code;
  r->relocs = enc.relocs;
  r->reloc_count = enc.reloc_count;
  return true;
}

bool nyr_x64_mach_append_function_debug(
    nyr_obj_buf_t *code, nyr_x64_obj_symbol_def_t *defs, size_t *def_count,
    nyr_x64_obj_reloc_t *relocs, size_t *reloc_count,
    const nyr_mach_func_t *mach, const ny_native_target_info_t *target,
    const char *symbol, bool tag_return, const nyr_func_t *source,
    nyr_native_debug_line_t **debug_lines, size_t *debug_line_count,
    size_t *debug_line_cap, char *err, size_t err_len) {
  if (!code || !defs || !def_count || !relocs || !reloc_count || !mach ||
      !symbol || !symbol[0]) {
    nyr_set_error(err, err_len, "x86-64 machine form append: missing input");
    return false;
  }
  nyr_mach_func_t mach_mut = *mach;
  (void)nyr_isle_apply_mach(&mach_mut);
  if (getenv("NY_DUMP_MACH")) {
    fprintf(stderr, "x86-64 encoded machine form for %s:\n", symbol);
    nyr_mach_dump(&mach_mut, stderr);
  }
  nyr_x64_mach_enc_t enc = {0};
  enc.mach = &mach_mut;
  enc.target = target;
  enc.err = err;
  enc.err_len = err_len;
  if (!mach_encode_function(&enc, symbol, tag_return)) {
    mach_free_remat_maps(&enc);
    free(enc.code.data);
    free(enc.relocs);
    free(enc.block_off);
    free(enc.patch_at);
    free(enc.patch_block);
    return false;
  }
  size_t start = code->len;
  if (source && debug_lines && debug_line_count && debug_line_cap) {
    for (size_t i = 0; i < mach_mut.inst_len; ++i) {
      size_t pc = enc.inst_offsets ? enc.inst_offsets[i] : SIZE_MAX;
      uint32_t source_pc = mach_mut.insts[i].source_pc;
      if (pc == SIZE_MAX || source_pc >= source->len ||
          !source->data[source_pc].debug.line)
        continue;
      if (*debug_line_count == *debug_line_cap) {
        size_t next = *debug_line_cap ? *debug_line_cap * 2 : 32;
        nyr_native_debug_line_t *grown =
            realloc(*debug_lines, next * sizeof(**debug_lines));
        if (!grown) {
          nyr_set_error(err, err_len,
                        "x86-64 machine form append: source map OOM");
          mach_free_remat_maps(&enc);
          free(enc.code.data);
          free(enc.relocs);
          free(enc.block_off);
          free(enc.patch_at);
          free(enc.patch_block);
          return false;
        }
        *debug_lines = grown;
        *debug_line_cap = next;
      }
      const nyr_debug_loc_t *loc = &source->data[source_pc].debug;
      (*debug_lines)[(*debug_line_count)++] = (nyr_native_debug_line_t){
          *def_count, start + pc, loc->file, loc->line, loc->column};
    }
  }
  mach_free_remat_maps(&enc);
  if (*def_count >= NY_NATIVE_MAX_DEFS ||
      !nyr_obj_emit(code, enc.code.data, enc.code.len)) {
    nyr_set_error(err, err_len,
                  "x86-64 machine form append: object buffer is full");
    free(enc.code.data);
    free(enc.relocs);
    free(enc.block_off);
    free(enc.patch_at);
    free(enc.patch_block);
    return false;
  }
  start = code->len - enc.code.len;
  snprintf(defs[*def_count].name, sizeof(defs[*def_count].name), "%s", symbol);
  defs[*def_count].off = start;
  defs[*def_count].size = enc.code.len;
  (*def_count)++;
  for (size_t i = 0; i < enc.reloc_count; ++i) {
    if (*reloc_count >= NY_X64_OBJ_MAX_RELOCS) {
      nyr_set_error(err, err_len,
                    "x86-64 machine form append: relocation limit exceeded");
      free(enc.code.data);
      free(enc.relocs);
      free(enc.block_off);
      free(enc.patch_at);
      free(enc.patch_block);
      return false;
    }
    relocs[*reloc_count] = enc.relocs[i];
    relocs[*reloc_count].disp_off += start;
    (*reloc_count)++;
  }
  free(enc.code.data);
  free(enc.relocs);
  free(enc.block_off);
  free(enc.patch_at);
  free(enc.patch_block);
  return true;
}

bool nyr_x64_mach_append_function(
    nyr_obj_buf_t *code, nyr_x64_obj_symbol_def_t *defs, size_t *def_count,
    nyr_x64_obj_reloc_t *relocs, size_t *reloc_count,
    const nyr_mach_func_t *mach, const ny_native_target_info_t *target,
    const char *symbol, bool tag_return, char *err, size_t err_len) {
  return nyr_x64_mach_append_function_debug(
      code, defs, def_count, relocs, reloc_count, mach, target, symbol,
      tag_return, NULL, NULL, NULL, NULL, err, err_len);
}

bool nyr_x64_mach_build_bundle(const nyr_mach_func_t *rt_main_mir,
                               const nyr_mach_func_t *func_mirs,
                               const char *const *func_names, size_t func_count,
                               const ny_native_target_info_t *target,
                               const char *entry_symbol, bool tag_return,
                               nyr_obj_buf_t *code,
                               nyr_x64_obj_symbol_def_t *defs,
                               size_t *def_count, nyr_x64_obj_reloc_t *relocs,
                               size_t *reloc_count, char *err, size_t err_len) {
  if (!rt_main_mir || !target || !code || !defs || !def_count || !relocs ||
      !reloc_count || !entry_symbol)
    return false;
  *def_count = 0;
  *reloc_count = 0;
  code->len = 0;

  size_t *order = NULL;
  ny_x64_parallel_encode_result_t *results = NULL;
  if (func_count) {
    order = malloc(func_count * sizeof(*order));
    results = calloc(func_count, sizeof(*results));
    if (!order || !results) {
      free(order);
      free(results);
      return false;
    }
    for (size_t i = 0; i < func_count; ++i)
      order[i] = i;
    for (size_t i = 0; i + 1 < func_count; ++i)
      for (size_t j = i + 1; j < func_count; ++j)
        if (func_mirs[order[j]].inst_len < func_mirs[order[i]].inst_len) {
          size_t t = order[i];
          order[i] = order[j];
          order[j] = t;
        }
    size_t work = 0;
    for (size_t i = 0; i < func_count; ++i)
      work += func_mirs[i].inst_len;
    ny_x64_parallel_encode_ctx_t ctx = {func_mirs, func_names, order, target,
                                        results};
    if (!ny_parallel_for(func_count, work, ny_x64_parallel_encode_task, &ctx)) {
      if (err && err_len)
        for (size_t i = 0; i < func_count; ++i)
          if (results[i].error[0]) {
            snprintf(err, err_len, "%s", results[i].error);
            break;
          }
      for (size_t i = 0; i < func_count; ++i) {
        nyr_obj_free(&results[i].code);
        free(results[i].relocs);
      }
      free(order);
      free(results);
      return false;
    }
    for (size_t oi = 0; oi < func_count; ++oi) {
      ny_x64_parallel_encode_result_t *r = &results[oi];
      size_t outlined_to = SIZE_MAX;
      if (target->outline_machine_functions && r->reloc_count == 0 &&
          r->code.len > 5) {
        for (size_t prior = 0; prior < oi; ++prior) {
          const ny_x64_parallel_encode_result_t *candidate = &results[prior];
          if (candidate->reloc_count == 0 &&
              candidate->code.len == r->code.len &&
              memcmp(candidate->code.data, r->code.data, r->code.len) == 0) {
            outlined_to = prior;
            break;
          }
        }
      }
      size_t start = code->len;
      size_t emitted = r->code.len;
      if (outlined_to != SIZE_MAX) {
        if (start > INT64_MAX - 5 || defs[outlined_to].off > INT64_MAX) {
          nyr_set_error(err, err_len,
                        "x86-64 machine outliner: code offset overflow");
          goto outline_fail;
        }
        int64_t delta = (int64_t)defs[outlined_to].off - (int64_t)(start + 5);
        if (delta < INT32_MIN || delta > INT32_MAX) {
          nyr_set_error(err, err_len,
                        "x86-64 machine outliner: thunk target out of range");
          goto outline_fail;
        }
        unsigned char thunk[5] = {0xe9};
        int32_t displacement = (int32_t)delta;
        memcpy(thunk + 1, &displacement, sizeof(displacement));
        if (!nyr_obj_emit(code, thunk, sizeof(thunk)))
          goto outline_fail;
        emitted = sizeof(thunk);
      } else if (!nyr_obj_emit(code, r->code.data, r->code.len)) {
        goto outline_fail;
      }
      if (*def_count >= NY_NATIVE_MAX_DEFS) {
        nyr_set_error(err, err_len,
                      "x86-64 machine form append: symbol limit exceeded");
        goto outline_fail;
      }
      snprintf(defs[*def_count].name, sizeof(defs[*def_count].name), "%s",
               r->symbol);
      defs[*def_count].off = start;
      defs[*def_count].size = emitted;
      (*def_count)++;
      if (outlined_to == SIZE_MAX) {
        for (size_t j = 0; j < r->reloc_count; ++j) {
          if (*reloc_count >= NY_X64_OBJ_MAX_RELOCS) {
            nyr_set_error(
                err, err_len,
                "x86-64 machine form append: relocation limit exceeded");
            goto outline_fail;
          }
          relocs[*reloc_count] = r->relocs[j];
          relocs[*reloc_count].disp_off += start;
          (*reloc_count)++;
        }
      }
    }
    for (size_t i = 0; i < func_count; ++i) {
      nyr_obj_free(&results[i].code);
      free(results[i].relocs);
    }
    free(order);
    free(results);
    order = NULL;
    results = NULL;
  }
  goto outlined_functions_done;

outline_fail:
  for (size_t i = 0; i < func_count; ++i) {
    nyr_obj_free(&results[i].code);
    free(results[i].relocs);
  }
  free(order);
  free(results);
  return false;

outlined_functions_done:;
  char entry[256];
  snprintf(entry, sizeof(entry), "%s%s",
           target->symbol_prefix ? target->symbol_prefix : "", entry_symbol);
  nyr_mach_func_t main_mut = *rt_main_mir;
  (void)nyr_isle_apply_mach(&main_mut);
  nyr_x64_mach_enc_t enc = {0};
  enc.mach = &main_mut;
  enc.target = target;
  enc.err = err;
  enc.err_len = err_len;
  size_t start = code->len;
  if (!mach_encode_function(&enc, entry, tag_return)) {
    mach_free_remat_maps(&enc);
    free(enc.code.data);
    free(enc.block_off);
    free(enc.patch_at);
    free(enc.patch_block);
    free(enc.relocs);
    return false;
  }
  mach_free_remat_maps(&enc);
  if (!nyr_obj_emit(code, enc.code.data, enc.code.len) ||
      *def_count >= NY_NATIVE_MAX_DEFS) {
    free(enc.code.data);
    free(enc.block_off);
    free(enc.patch_at);
    free(enc.patch_block);
    free(enc.relocs);
    return false;
  }
  snprintf(defs[*def_count].name, sizeof(defs[*def_count].name), "%s", entry);
  defs[*def_count].off = start;
  defs[*def_count].size = enc.code.len;
  (*def_count)++;
  for (size_t r = 0; r < enc.reloc_count; ++r) {
    if (*reloc_count >= NY_X64_OBJ_MAX_RELOCS) {
      free(enc.code.data);
      free(enc.block_off);
      free(enc.patch_at);
      free(enc.patch_block);
      free(enc.relocs);
      return false;
    }
    relocs[*reloc_count] = enc.relocs[r];
    relocs[*reloc_count].disp_off += start;
    (*reloc_count)++;
  }
  free(enc.code.data);
  free(enc.block_off);
  free(enc.patch_at);
  free(enc.patch_block);
  free(enc.relocs);
  if (!nyr_strtab_append_defs(code, defs, def_count, err, err_len))
    return false;
  if (!nyr_consttab_append_defs(code, defs, def_count, err, err_len))
    return false;
  if (!nyr_globaltab_append_defs(code, defs, def_count, err, err_len))
    return false;
  return nyr_arraytab_append_defs(code, defs, def_count, err, err_len);
}

/*
 * Copy-and-patch style stencils for the absolute smallest programs.
 * Template bytes with holes patched for immediates only — no full RA.
 * Covers: const ret, and a measured fast path for pure foldable i64.
 */

static bool ny_x64_stencil_const_i64(nyr_obj_buf_t *code, int64_t imm,
                                     nyr_x64_obj_symbol_def_t *defs,
                                     size_t *def_count, const char *symbol) {
  if (!code || !defs || !def_count || !symbol)
    return false;
  size_t start = code->len;
  /*
   * push rbp; mov rbp,rsp; movabs rax,imm; leave; ret
   */
  unsigned char prolog[] = {0x55, 0x48, 0x89, 0xe5, 0x48, 0xb8};
  if (!nyr_obj_emit(code, prolog, sizeof(prolog)))
    return false;
  for (int i = 0; i < 8; ++i) {
    unsigned char b = (unsigned char)(((uint64_t)imm >> (i * 8)) & 0xff);
    if (!nyr_obj_emit(code, &b, 1))
      return false;
  }
  unsigned char ep[] = {0xc9, 0xc3};
  if (!nyr_obj_emit(code, ep, sizeof(ep)))
    return false;
  if (*def_count >= NY_NATIVE_MAX_DEFS)
    return false;
  snprintf(defs[*def_count].name, sizeof(defs[*def_count].name), "%s", symbol);
  defs[*def_count].off = start;
  defs[*def_count].size = code->len - start;
  (*def_count)++;
  return true;
}

/*
 * Local-slot const: store imm to [rbp-8], load back, ret — exercises local
 * stencil shape used by small programs with one local.
 */
static bool ny_x64_stencil_local_i64(nyr_obj_buf_t *code, int64_t imm,
                                     nyr_x64_obj_symbol_def_t *defs,
                                     size_t *def_count, const char *symbol) {
  if (!code || !defs || !def_count || !symbol)
    return false;
  size_t start = code->len;
  /*
   * push rbp; mov rbp,rsp; sub rsp,16; movabs rax,imm; mov [rbp-8],rax;
   * mov rax,[rbp-8]; leave; ret
   */
  unsigned char bytes[] = {0x55, 0x48, 0x89, 0xe5, 0x48,
                           0x83, 0xec, 0x10, 0x48, 0xb8};
  if (!nyr_obj_emit(code, bytes, sizeof(bytes)))
    return false;
  for (int i = 0; i < 8; ++i) {
    unsigned char b = (unsigned char)(((uint64_t)imm >> (i * 8)) & 0xff);
    if (!nyr_obj_emit(code, &b, 1))
      return false;
  }
  unsigned char mid[] = {0x48, 0x89, 0x45, 0xf8, 0x48,
                         0x8b, 0x45, 0xf8, 0xc9, 0xc3};
  if (!nyr_obj_emit(code, mid, sizeof(mid)))
    return false;
  if (*def_count >= NY_NATIVE_MAX_DEFS)
    return false;
  snprintf(defs[*def_count].name, sizeof(defs[*def_count].name), "%s", symbol);
  defs[*def_count].off = start;
  defs[*def_count].size = code->len - start;
  (*def_count)++;
  return true;
}

static bool ny_x64_stencil_fold_shift(nyr_op_t op, int64_t a, int64_t b,
                                      int64_t *out) {
  if (!out || b < 0 || b > (op == NYIR_ROR32_I64 ? 31 : 63))
    return false;
  unsigned shift = (unsigned)b;
  switch (op) {
  case NYIR_SHL_I64:
    *out = (int64_t)((uint64_t)a << shift);
    return true;
  case NYIR_SAR_I64:
    *out = a >> shift;
    return true;
  case NYIR_ROR_I64:
    *out =
        shift == 0
            ? a
            : (int64_t)(((uint64_t)a >> shift) | ((uint64_t)a << (64 - shift)));
    return true;
  case NYIR_ROR32_I64: {
    uint32_t value = (uint32_t)a;
    *out =
        (int64_t)(shift == 0
                      ? value
                      : (uint32_t)((value >> shift) | (value << (32 - shift))));
    return true;
  }
  default:
    return false;
  }
}

/*
 * Fold a pure helper body with known integer args into a single result.
 * Rejects mem/extern/PHI/indirect. Returns false if not fully foldable.
 */
static bool ny_x64_stencil_fold_helper(const nyr_func_t *fn,
                                       const int64_t *args, int argc,
                                       int64_t *out) {
  if (!fn || !out || argc < 0 || argc > 8)
    return false;
  int64_t *val = NULL;
  bool *known = NULL;
  int64_t locals[64];
  bool local_known[64];
  memset(local_known, 0, sizeof(local_known));
  memset(locals, 0, sizeof(locals));
  if (fn->next_value > 0) {
    val = calloc((size_t)fn->next_value, sizeof(int64_t));
    known = calloc((size_t)fn->next_value, sizeof(bool));
    if (!val || !known) {
      free(val);
      free(known);
      return false;
    }
  }
  /*
   * Params: first argc LOAD_LOCAL of local#0.. or param convention as first
   * values — NYIR uses load.local for params after store at entry. Scan for
   * store.local of const/args pattern; simpler: seed locals[0..argc) = args.
   */
  for (int i = 0; i < argc && i < 64; ++i) {
    locals[i] = args[i];
    local_known[i] = true;
  }
  size_t *lab_at = NULL;
  size_t lab_cap = 0;
  for (size_t i = 0; i < fn->len; ++i)
    if (fn->data[i].op == NYIR_LABEL && fn->data[i].imm >= 0 &&
        (size_t)fn->data[i].imm + 1 > lab_cap)
      lab_cap = (size_t)fn->data[i].imm + 1;
  if (lab_cap) {
    lab_at = malloc(lab_cap * sizeof(size_t));
    if (!lab_at) {
      free(val);
      free(known);
      return false;
    }
    for (size_t i = 0; i < lab_cap; ++i)
      lab_at[i] = (size_t)-1;
    for (size_t i = 0; i < fn->len; ++i)
      if (fn->data[i].op == NYIR_LABEL && fn->data[i].imm >= 0)
        lab_at[fn->data[i].imm] = i;
  }
  size_t pc = 0, steps = 0;
  const size_t max_steps = fn->len * 4 + 64;
  bool ok = false;
  while (pc < fn->len && steps++ < max_steps) {
    const nyr_inst_t *in = &fn->data[pc];
    if (in->op == NYIR_NOP || in->op == NYIR_LABEL) {
      pc++;
      continue;
    }
    if (in->op == NYIR_CALL || in->op == NYIR_LOAD_I64 ||
        in->op == NYIR_STORE_I64 || in->op == NYIR_PHI ||
        in->op == NYIR_ADDR_LOCAL || in->op == NYIR_ALLOCA)
      break;
    if (in->op == NYIR_CONST_I64 && in->dst >= 0) {
      known[in->dst] = true;
      val[in->dst] = in->imm;
      pc++;
      continue;
    }
    if (in->op == NYIR_COPY && in->dst >= 0 && in->a >= 0 && known &&
        known[in->a]) {
      known[in->dst] = true;
      val[in->dst] = val[in->a];
      pc++;
      continue;
    }
    if (in->op == NYIR_STORE_LOCAL && in->imm >= 0 && in->imm < 64 &&
        in->a >= 0 && known && known[in->a]) {
      locals[in->imm] = val[in->a];
      local_known[in->imm] = true;
      pc++;
      continue;
    }
    if (in->op == NYIR_LOAD_LOCAL && in->imm >= 0 && in->imm < 64 &&
        local_known[in->imm] && in->dst >= 0) {
      known[in->dst] = true;
      val[in->dst] = locals[in->imm];
      pc++;
      continue;
    }
    if (in->dst >= 0 && in->a >= 0 && known && known[in->a] &&
        (in->b < 0 || known[in->b])) {
      int64_t a = val[in->a], b = in->b >= 0 ? val[in->b] : 0, r = 0;
      bool okf = true;
      switch (in->op) {
      case NYIR_ADD_I64:
        r = a + b;
        break;
      case NYIR_SUB_I64:
        r = a - b;
        break;
      case NYIR_MUL_I64:
        r = a * b;
        break;
      case NYIR_AND_I64:
        r = a & b;
        break;
      case NYIR_OR_I64:
        r = a | b;
        break;
      case NYIR_XOR_I64:
        r = a ^ b;
        break;
      case NYIR_BSWAP_I64:
        r = (int64_t)__builtin_bswap64((uint64_t)a);
        break;
      case NYIR_BSWAP32_I64:
        r = (int64_t)(uint32_t)__builtin_bswap32((uint32_t)a);
        break;
      case NYIR_SHL_I64:
      case NYIR_SAR_I64:
      case NYIR_ROR32_I64:
      case NYIR_ROR_I64:
        okf = ny_x64_stencil_fold_shift(in->op, a, b, &r);
        break;
      case NYIR_DIV_I64:
        if (!b) {
          okf = false;
          break;
        }
        r = a / b;
        break;
      case NYIR_MOD_I64:
        if (!b) {
          okf = false;
          break;
        }
        r = a % b;
        break;
      case NYIR_CMP_I64:
        switch (in->cmp) {
        case NYIR_CMP_EQ:
          r = a == b;
          break;
        case NYIR_CMP_NE:
          r = a != b;
          break;
        case NYIR_CMP_LT:
          r = a < b;
          break;
        case NYIR_CMP_LE:
          r = a <= b;
          break;
        case NYIR_CMP_GT:
          r = a > b;
          break;
        case NYIR_CMP_GE:
          r = a >= b;
          break;
        default:
          okf = false;
          break;
        }
        break;
      default:
        okf = false;
        break;
      }
      if (okf) {
        known[in->dst] = true;
        val[in->dst] = r;
        pc++;
        continue;
      }
    }
    if (in->op == NYIR_BR) {
      if (!lab_at || in->imm < 0 || (size_t)in->imm >= lab_cap ||
          lab_at[in->imm] == (size_t)-1)
        break;
      pc = lab_at[in->imm];
      continue;
    }
    if (in->op == NYIR_BR_IF) {
      if (in->a < 0 || !known || !known[in->a])
        break;
      if (val[in->a]) {
        if (!lab_at || in->imm < 0 || (size_t)in->imm >= lab_cap ||
            lab_at[in->imm] == (size_t)-1)
          break;
        pc = lab_at[in->imm];
      } else
        pc++;
      continue;
    }
    if (in->op == NYIR_RET && in->a >= 0 && known && known[in->a]) {
      *out = val[in->a];
      ok = true;
      break;
    }
    break;
  }
  free(lab_at);
  free(val);
  free(known);
  return ok;
}

/*
 * Try stencil for pure rt_main: const-foldable i64 including diamond CFG
 * when all branch conditions fold (br/br_if/label walk). Optionally folds
 * pure direct calls into helper bodies (call stencil).
 */
static bool ny_x64_try_stencil_bundle_impl(
    const nyr_func_t *rt_main, const nyr_func_t *funcs,
    const char *const *func_names, size_t func_count,
    const ny_native_target_info_t *target, nyr_obj_buf_t *code,
    nyr_x64_obj_symbol_def_t *defs, size_t *def_count,
    nyr_x64_obj_reloc_t *relocs, size_t *reloc_count, char *err,
    size_t err_len) {
  (void)relocs;
  (void)err;
  (void)err_len;
  if (!rt_main || !target || !code || !defs || !def_count || !reloc_count)
    return false;
  *def_count = 0;
  *reloc_count = 0;
  code->len = 0;
  bool allow_calls = funcs && func_names && func_count > 0;
  /*
   * Reject mem/PHI; CALL only when helpers available for pure fold.
   */
  for (size_t i = 0; i < rt_main->len; ++i) {
    nyr_op_t op = rt_main->data[i].op;
    if (op == NYIR_LOAD_I64 || op == NYIR_STORE_I64 || op == NYIR_PHI ||
        op == NYIR_ADDR_LOCAL || op == NYIR_ALLOCA)
      return false;
    if (op == NYIR_CALL) {
      if (!allow_calls || (rt_main->data[i].flags & NYIR_INST_F_EXTERN))
        return false;
    }
  }
  int64_t *val = NULL;
  bool *known = NULL;
  int64_t locals[64];
  bool local_known[64];
  memset(local_known, 0, sizeof(local_known));
  memset(locals, 0, sizeof(locals));
  if (rt_main->next_value > 0) {
    val = calloc((size_t)rt_main->next_value, sizeof(int64_t));
    known = calloc((size_t)rt_main->next_value, sizeof(bool));
    if (!val || !known) {
      free(val);
      free(known);
      return false;
    }
  }
  /*
   * Map label id → instruction index.
   */
  size_t *lab_at = NULL;
  size_t lab_cap = 0;
  for (size_t i = 0; i < rt_main->len; ++i)
    if (rt_main->data[i].op == NYIR_LABEL && rt_main->data[i].imm >= 0 &&
        (size_t)rt_main->data[i].imm + 1 > lab_cap)
      lab_cap = (size_t)rt_main->data[i].imm + 1;
  if (lab_cap) {
    lab_at = malloc(lab_cap * sizeof(size_t));
    if (!lab_at) {
      free(val);
      free(known);
      return false;
    }
    for (size_t i = 0; i < lab_cap; ++i)
      lab_at[i] = (size_t)-1;
    for (size_t i = 0; i < rt_main->len; ++i)
      if (rt_main->data[i].op == NYIR_LABEL && rt_main->data[i].imm >= 0)
        lab_at[rt_main->data[i].imm] = i;
  }
  size_t pc = 0;
  size_t steps = 0;
  const size_t max_steps = rt_main->len * 4 + 64;
  while (pc < rt_main->len && steps++ < max_steps) {
    const nyr_inst_t *in = &rt_main->data[pc];
    if (in->op == NYIR_NOP || in->op == NYIR_LABEL) {
      pc++;
      continue;
    }
    if (in->op == NYIR_CONST_I64 && in->dst >= 0) {
      known[in->dst] = true;
      val[in->dst] = in->imm;
      pc++;
      continue;
    }
    if (in->op == NYIR_COPY && in->dst >= 0 && in->a >= 0 && known &&
        known[in->a]) {
      known[in->dst] = true;
      val[in->dst] = val[in->a];
      pc++;
      continue;
    }
    if (in->op == NYIR_STORE_LOCAL && in->imm >= 0 && in->imm < 64 &&
        in->a >= 0 && known && known[in->a]) {
      locals[in->imm] = val[in->a];
      local_known[in->imm] = true;
      pc++;
      continue;
    }
    if (in->op == NYIR_LOAD_LOCAL && in->imm >= 0 && in->imm < 64 &&
        local_known[in->imm] && in->dst >= 0) {
      known[in->dst] = true;
      val[in->dst] = locals[in->imm];
      pc++;
      continue;
    }
    if (in->dst >= 0 && in->a >= 0 && known && known[in->a] &&
        (in->b < 0 || known[in->b])) {
      int64_t a = val[in->a];
      int64_t b = in->b >= 0 ? val[in->b] : 0;
      int64_t r = 0;
      bool okf = true;
      switch (in->op) {
      case NYIR_ADD_I64:
        r = a + b;
        break;
      case NYIR_SUB_I64:
        r = a - b;
        break;
      case NYIR_MUL_I64:
        r = a * b;
        break;
      case NYIR_AND_I64:
        r = a & b;
        break;
      case NYIR_OR_I64:
        r = a | b;
        break;
      case NYIR_XOR_I64:
        r = a ^ b;
        break;
      case NYIR_BSWAP_I64:
        r = (int64_t)__builtin_bswap64((uint64_t)a);
        break;
      case NYIR_BSWAP32_I64:
        r = (int64_t)(uint32_t)__builtin_bswap32((uint32_t)a);
        break;
      case NYIR_SHL_I64:
      case NYIR_SAR_I64:
      case NYIR_ROR32_I64:
      case NYIR_ROR_I64:
        okf = ny_x64_stencil_fold_shift(in->op, a, b, &r);
        break;
      case NYIR_DIV_I64:
        if (b == 0) {
          okf = false;
          break;
        }
        r = a / b;
        break;
      case NYIR_MOD_I64:
        if (b == 0) {
          okf = false;
          break;
        }
        r = a % b;
        break;
      case NYIR_CMP_I64:
        switch (in->cmp) {
        case NYIR_CMP_EQ:
          r = a == b;
          break;
        case NYIR_CMP_NE:
          r = a != b;
          break;
        case NYIR_CMP_LT:
          r = a < b;
          break;
        case NYIR_CMP_LE:
          r = a <= b;
          break;
        case NYIR_CMP_GT:
          r = a > b;
          break;
        case NYIR_CMP_GE:
          r = a >= b;
          break;
        default:
          okf = false;
          break;
        }
        break;
      default:
        okf = false;
        break;
      }
      if (okf) {
        known[in->dst] = true;
        val[in->dst] = r;
        pc++;
        continue;
      }
    }
    if (in->op == NYIR_CALL) {
      /*
       * Reject any call the fold cannot consume: either this branch folds a
       * known pure helper (and continues) or the stencil must fail instead
       * of silently dropping a side-effecting call.
       */
      if (!allow_calls || !in->symbol)
        goto stencil_unfoldable_call;
    }
    if (in->op == NYIR_CALL && allow_calls && in->symbol) {
      /*
       * Resolve helper by name; fold with known args.  Any call we cannot
       * fold must fail the whole stencil rather than be dropped: silently
       * skipping it emits a shell that looks self-contained (reloc_count 0)
       * but never performs the side effect.
       */
      const nyr_func_t *callee = NULL;
      for (size_t fi = 0; fi < func_count; ++fi) {
        if (func_names[fi] && strcmp(func_names[fi], in->symbol) == 0) {
          callee = &funcs[fi];
          break;
        }
      }
      if (!callee)
        goto stencil_unfoldable_call;
      int carg[8];
      int cargc = 0;
      if (!nyr_call_args(in, rt_main->next_value, carg, 8, &cargc, NULL, 0))
        goto stencil_unfoldable_call;
      int64_t avals[8];
      bool args_ok = true;
      for (int ai = 0; ai < cargc; ++ai) {
        if (carg[ai] < 0 || !known || !known[carg[ai]]) {
          args_ok = false;
          break;
        }
        avals[ai] = val[carg[ai]];
      }
      if (!args_ok)
        goto stencil_unfoldable_call;
      int64_t r = 0;
      if (!ny_x64_stencil_fold_helper(callee, avals, cargc, &r))
        goto stencil_unfoldable_call;
      if (in->dst >= 0) {
        known[in->dst] = true;
        val[in->dst] = r;
      }
      pc++;
      continue;
    }
    if (in->op == NYIR_BR) {
      if (!lab_at || in->imm < 0 || (size_t)in->imm >= lab_cap ||
          lab_at[in->imm] == (size_t)-1)
        break;
      pc = lab_at[in->imm];
      continue;
    }
    if (in->op == NYIR_BR_IF) {
      if (in->a < 0 || !known || !known[in->a])
        break;
      if (val[in->a]) {
        if (!lab_at || in->imm < 0 || (size_t)in->imm >= lab_cap ||
            lab_at[in->imm] == (size_t)-1)
          break;
        pc = lab_at[in->imm];
      } else {
        pc++;
      }
      continue;
    }
    if (in->op == NYIR_RET && in->a >= 0 && known && known[in->a]) {
      char entry[256];
      snprintf(entry, sizeof(entry), "%srt_main",
               target->symbol_prefix ? target->symbol_prefix : "");
      /*
       * Prefer local stencil shell when IR used a local (store/load pattern).
       */
      bool used_local = false;
      for (size_t j = 0; j < rt_main->len; ++j)
        if (rt_main->data[j].op == NYIR_STORE_LOCAL ||
            rt_main->data[j].op == NYIR_LOAD_LOCAL) {
          used_local = true;
          break;
        }
      bool ok = used_local ? ny_x64_stencil_local_i64(code, val[in->a], defs,
                                                      def_count, entry)
                           : ny_x64_stencil_const_i64(code, val[in->a], defs,
                                                      def_count, entry);
      free(lab_at);
      free(val);
      free(known);
      return ok;
    }
    break;
  }
  free(lab_at);
  free(val);
  free(known);
  return false;
stencil_unfoldable_call:
  free(lab_at);
  free(val);
  free(known);
  return false;
}

bool nyr_x64_try_stencil_bundle(const nyr_func_t *rt_main,
                                const ny_native_target_info_t *target,
                                nyr_obj_buf_t *code,
                                nyr_x64_obj_symbol_def_t *defs,
                                size_t *def_count, nyr_x64_obj_reloc_t *relocs,
                                size_t *reloc_count, char *err,
                                size_t err_len) {
  return ny_x64_try_stencil_bundle_impl(rt_main, NULL, NULL, 0, target, code,
                                        defs, def_count, relocs, reloc_count,
                                        err, err_len);
}

bool nyr_x64_try_stencil_bundle_calls(
    const nyr_func_t *rt_main, const nyr_func_t *funcs,
    const char *const *func_names, size_t func_count,
    const ny_native_target_info_t *target, nyr_obj_buf_t *code,
    nyr_x64_obj_symbol_def_t *defs, size_t *def_count,
    nyr_x64_obj_reloc_t *relocs, size_t *reloc_count, char *err,
    size_t err_len) {
  return ny_x64_try_stencil_bundle_impl(rt_main, funcs, func_names, func_count,
                                        target, code, defs, def_count, relocs,
                                        reloc_count, err, err_len);
}
