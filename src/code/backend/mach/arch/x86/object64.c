/*
 * x86-64 machine-form encoder: register allocation, instruction bytes,
 * relocations, and SSE/AVX encodings.
 */
#include "base/compat.h"
#include "base/debug.h"
#include "base/parallel.h"
#include "code/backend/core/abi.h"
#include "code/backend/lower/machine.h"
#include "code/backend/mach/arch/x86/retpoline.h"
#include "code/backend/mach/bare/payload.h"
#include "code/backend/mach/object.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

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
  /*
   * Parallel to patch_at: 1 when the branch was emitted in rel8 form, so the
   * patch site holds one displacement byte instead of four.
   */
  unsigned char *patch_short;
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
  /*
   * Vreg the sticky tag describes, -1 when the producer knew only an offset.
   * A pooled home slot needs this: the offset alone cannot say whose bits RAX
   * holds, so a shared slot only reuses the tag for the same vreg.
   */
  int last_rax_vreg;
  /*
   * Last complete stack-home store. Byte adjacency proves that its source
   * register and destination memory are unchanged before a repeated access.
   */
  size_t last_home_store_end;
  int last_home_store_off;
  int last_home_store_preg;
  int pending_rax_off;
  bool pending_rax_valid;
  int pending_rax_vreg;
  uint32_t r10_index_vreg;
  bool r10_index_valid;
  uint32_t pending_sib_vreg;
  unsigned pending_sib_base;
  unsigned pending_sib_index;
  unsigned pending_sib_scale;
  bool pending_sib_valid;
  /*
   * SHL x, k (k = 1..3) folded into the next ADD as lea [y + x*2^k].
   */
  bool shl_fuse_valid;
  size_t shl_fuse_add;
  int shl_fuse_index;
  unsigned shl_fuse_scale;
  uint32_t shl_fuse_dst;
  /*
   * Home of a destination that is also an input of this instruction.
   */
  int aliased_dst_home;
  /*
   * Code position when RAX was last known to equal last_rax_off's slot.
   */
  size_t last_rax_sync_end;
  /*
   * Values with a segment starting or ending at each instruction.
   */
  uint32_t *seg_start_offsets, *seg_start_vregs;
  uint32_t *seg_end_offsets, *seg_end_vregs;
  nyr_mach_regalloc_t regalloc;
  /*
   * Set for throwaway probe encodes (home dead-store analysis).  A probe must
   * not print an audit line and must not add its segments to the global
   * regalloc counters, or every function is counted twice.
   */
  bool suppress_stats;
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
   * seed. This path remains opt-in until the native suite validates the
   * encoder edge proof for all loop-carried values.
   */
  bool loop_carry_seeding;
  /*
   * Elide a register copy whose source and destination already share a
   * physical color instead of staging it through %rax. Measured on `knight`:
   * 498 -> 418 instructions and 5240 -> 5016 bytes of `moves` text.
   * NYTRIX_NO_SAME_COLOR_COPY restores the staging path for A/B runs.
   */
  bool same_color_copy_fold;
  /*
   * Materialize a symbol address directly in the LEA destination's assigned
   * color instead of staging it through %rax. Measured on `knight`: 4584 ->
   * 4320 bytes of text. NYTRIX_NO_LEA_COLOR restores the RAX staging path
   * for A/B runs.
   */
  bool lea_assign_color;
  /*
   * Operand reference counts per vreg.  A symbol LEA whose result has exactly
   * one use (the COPY that immediately follows) can be materialized straight
   * into that copy's register instead of through %rax.
   */
  uint32_t *vreg_use_count;
  /*
   * Encoder decision counters for the audit: how symbol addresses were
   * materialized.  These are what an address-encoding change moves, so they
   * are the evidence a size delta is attributed to, rather than an inference
   * from reading assembly.
   */
  unsigned stat_lea_color;
  unsigned stat_lea_staged;
  /*
   * Vector loop carries have a separate allocator proof and XMM/YMM seeding
   * path, so they can be enabled without opting GPR/FPR carries in.
   */
  bool vector_loop_carry_seeding;
  unsigned char
      fused_jcc; /* flags-only CMP jcc opcode for the next BR_IF (0=none) */
  /*
   * Callee-save push count (r12-r15/rbx). Frame/vreg homes sit below them.
   */
  unsigned save_mask;
  int cs_slots;
  nyr_mach_regalloc_t fp_regalloc;
  bool *fp_color_seeded;
  /*
   * Per float vreg, 1 + the instruction that last loaded its register from
   * the home (fp_seed_inst) or last named it as a destination
   * (fp_last_def_inst); 0 when none. A register seeded after its last
   * definition in the current block still equals the home, so a flush need
   * not store it back.
   */
  size_t *fp_seed_inst;
  size_t *fp_last_def_inst;
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
   * Per-instruction segment indexes for the independent float and vector
   * allocators; the scalar seg_start/seg_end tables cover e->regalloc only.
   */
  uint32_t *fp_seg_start_offsets, *fp_seg_start_vregs;
  uint32_t *fp_seg_end_offsets, *fp_seg_end_vregs;
  uint32_t *vec_seg_start_offsets, *vec_seg_start_vregs;
  uint32_t *vec_seg_end_offsets, *vec_seg_end_vregs;
  /*
   * Allocatable vector colors for this function: MVEC_COLOR_N, or
   * MVEC_COLOR_WIDE_N when no scalar float value can claim xmm4..xmm7.
   */
  size_t vec_color_n;
  /*
   * The function has 256-bit values; see mach_vzeroupper.
   */
  bool uses_ymm;
  /*
   * Per block: 1 when the upper YMM state may be dirty on entry. Built on
   * first use by mach_block_vzeroupper.
   */
  unsigned char *ymm_dirty_in;
  /*
   * Per block: required byte alignment of its entry, 0 for none. Set only on
   * backedge targets; see mach_build_block_align.
   */
  unsigned char *block_align;
  /*
   * AVX target: scalar/128-bit SSE helpers emit VEX forms (mach_vex).
   */
  bool vex;
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
  unsigned char *vreg_home_shared; /* set when a home slot is shared */
  /*
   * Offsets owned by more than one vreg, sorted for bsearch. The sticky-RAX
   * tag names an offset, so a shared slot cannot use it; see
   * mach_home_off_shared.
   */
  int *shared_home_offs;
  size_t shared_home_off_len;
  /*
   * Literals uncolored by mach_decolor_literals (uses rematerialize the
   * immediate); see the single-consumer publish skip in the COPY encoder.
   */
  bool *decolored_literal;
  int *frame_slot_offsets;
  size_t vreg_home_bytes;
  /*
   * Defining instruction index per vreg (SIZE_MAX when none), built once
   * per function for address-mode decomposition.
   */
  size_t *def_inst;
  /*
   * Definitions per vreg, saturating at 2, and whether a single-definition
   * vector's stack home was written in its defining block. The defining
   * block dominates every use, so such a home stays current everywhere.
   */
  uint8_t *def_count;
  /*
   * Per GPR vreg: its home was written after its only definition, inside the
   * defining block. See mach_publish_gpr_home.
   */
  bool *gpr_home_stored;
  bool *vec_home_stored;
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
  /*
   * CMP feeding only a SELECT in its block: its index, re-emitted there.
   */
  size_t *select_cmp_inst;
  uint8_t *cmp_elision_action;
  uint8_t *select_skip_alu;
  int8_t *direct_param_source;
  bool *direct_param_slots;
  bool bare_syscalls;
  /*
   * Encoder dry runs used by dead-home analysis must not pollute metrics.
   */
  bool suppress_div_stats;
  unsigned long long suppressed_div_magic;
  unsigned long long suppressed_div_idiv;
  size_t bare_payload_entry; /* code offset + 1; zero means not emitted */
  /*
   * Dead spill-home elimination.  A first encoding counts the scalar home
   * stores made per frame offset; mach_home_dse_analyze then scans the bytes
   * it produced.  An offset is dropped only when those stores are the only
   * references to it, so an access the encoder reaches by any other route
   * keeps its stores.
   */
  int *home_offs_snapshot; /* first pass only; maps are torn down on return */
  uint32_t *home_store_counts; /* first pass, indexed by -off */
  bool home_store_seen;
  /*
   * Emitted home traffic, counted in the final pass only.
   */
  size_t home_store_ops;
  size_t home_load_ops;
  size_t frame_ops;
  size_t home_fold_ops;
  size_t alu_commute_ops;
  const unsigned char *home_dead; /* second pass, indexed by -off */
  size_t home_limit;
  /*
   * Per vreg, 1 + the last instruction that loads a float through it or
   * stores a float through it (0: none); built on first use for `mach`.
   */
  size_t *float_mem_last;
  const nyr_mach_inst_t *float_mem_insts;
  size_t float_mem_len;
} nyr_x64_mach_enc_t;

static void mach_rax_invalidate(nyr_x64_mach_enc_t *e) {
  e->last_rax_valid = false;
  e->last_rax_off = 0;
  e->last_rax_vreg = -1;
}

static void mach_r10_index_invalidate(nyr_x64_mach_enc_t *e) {
  e->r10_index_valid = false;
}

static void mach_record_div_stat(nyr_x64_mach_enc_t *e, bool magic) {
  if (e && e->suppress_div_stats) {
    if (magic)
      ++e->suppressed_div_magic;
    else
      ++e->suppressed_div_idiv;
    return;
  }
  nyr_native_mach_div_record(magic);
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
  if (divisor <= 1 || divisor > (INT64_C(1) << 31) ||
      ((uint64_t)divisor & ((uint64_t)divisor - 1)) != 0)
    return false;
  /*
   * The signed remainder bias uses a zero-extended mask encoded as a signed
   * imm32, so shift 31 is the maximum supported value.
   */
  unsigned shift = (unsigned)__builtin_ctzll((unsigned long long)divisor);
  if (shift > 31)
    return false;
  *shift_out = shift;
  return true;
}

static void mach_free_remat_maps(nyr_x64_mach_enc_t *e) {
  if (!e)
    return;
  free((void *)e->imm_fold);
  free((void *)e->imm_fold_known);
  free((void *)e->addr_fold);
  free((void *)e->addr_fold_known);
  free(e->vreg_home_offs);
  free(e->decolored_literal);
  e->decolored_literal = NULL;
  free(e->frame_slot_offsets);
  e->imm_fold = NULL;
  e->imm_fold_known = NULL;
  e->addr_fold = NULL;
  e->addr_fold_known = NULL;
  e->vreg_home_offs = NULL;
  e->vreg_home_shared = NULL;
  free(e->shared_home_offs);
  e->shared_home_offs = NULL;
  e->shared_home_off_len = 0;
  e->frame_slot_offsets = NULL;
  e->vreg_home_bytes = 0;
  free(e->inst_offsets);
  e->inst_offsets = NULL;
  free(e->vreg_has_ret_use);
  free(e->vreg_last_call_arg_inst);
  free(e->vreg_literal_rhs_only);
  free(e->vreg_use_count);
  e->vreg_use_count = NULL;
  free(e->vreg_carried_copy_cache);
  free(e->loop_fpr);
  free(e->loop_fpr_entry);
  e->loop_fpr = NULL;
  e->loop_fpr_entry = NULL;
  free(e->seg_start_offsets);
  free(e->seg_start_vregs);
  free(e->seg_end_offsets);
  free(e->seg_end_vregs);
  e->seg_start_offsets = e->seg_start_vregs = NULL;
  e->seg_end_offsets = e->seg_end_vregs = NULL;
  free(e->fp_seg_start_offsets);
  free(e->fp_seg_start_vregs);
  free(e->fp_seg_end_offsets);
  free(e->fp_seg_end_vregs);
  free(e->vec_seg_start_offsets);
  free(e->vec_seg_start_vregs);
  free(e->vec_seg_end_offsets);
  free(e->vec_seg_end_vregs);
  e->fp_seg_start_offsets = e->fp_seg_start_vregs = NULL;
  e->fp_seg_end_offsets = e->fp_seg_end_vregs = NULL;
  e->vec_seg_start_offsets = e->vec_seg_start_vregs = NULL;
  e->vec_seg_end_offsets = e->vec_seg_end_vregs = NULL;
  free(e->loop_gpr);
  e->loop_gpr = NULL;
  free(e->select_test_src);
  free(e->select_test_equal);
  free(e->select_cmp_inst);
  free(e->cmp_elision_action);
  free(e->select_skip_alu);
  free(e->direct_param_source);
  free(e->direct_param_slots);
  e->vreg_has_ret_use = NULL;
  e->vreg_last_call_arg_inst = NULL;
  e->vreg_literal_rhs_only = NULL;
  e->vreg_carried_copy_cache = NULL;
  e->select_test_src = NULL;
  e->select_test_equal = NULL;
  e->select_cmp_inst = NULL;
  e->cmp_elision_action = NULL;
  e->select_skip_alu = NULL;
  e->direct_param_source = NULL;
  e->direct_param_slots = NULL;
}

static void mach_enc_free_scratch(nyr_x64_mach_enc_t *e) {
  free(e->block_off);
  free(e->ymm_dirty_in);
  e->ymm_dirty_in = NULL;
  free(e->block_align);
  e->block_align = NULL;
  free(e->patch_at);
  free(e->patch_short);
  e->patch_short = NULL;
  free(e->patch_block);
  e->block_off = NULL;
  e->patch_at = NULL;
  e->patch_block = NULL;
}

static void mach_enc_dispose(nyr_x64_mach_enc_t *e) {
  if (!e)
    return;
  free(e->float_mem_last);
  e->float_mem_last = NULL;
  e->float_mem_insts = NULL;
  mach_free_remat_maps(e);
  free(e->code.data);
  free(e->relocs);
  e->code.data = NULL;
  e->relocs = NULL;
  mach_enc_free_scratch(e);
}

static void mach_free_remat_maps_preserving_offsets(nyr_x64_mach_enc_t *e) {
  if (!e)
    return;
  size_t *inst_offsets = e->inst_offsets;
  e->inst_offsets = NULL;
  mach_free_remat_maps(e);
  e->inst_offsets = inst_offsets;
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
  if (n <= e->code.cap && e->code.len <= e->code.cap - n &&
      (!n || (e->code.data && p))) {
    if (n)
      memcpy(e->code.data + e->code.len, p, n);
    e->code.len += n;
    return true;
  }
  if (!nyr_obj_emit(&e->code, p, n))
    return mach_err(e, "x64 machine form encode: out of memory");
  return true;
}

static bool mach_u8(nyr_x64_mach_enc_t *e, unsigned v) {
  if (e->code.data && e->code.len < e->code.cap) {
    e->code.data[e->code.len++] = (unsigned char)v;
    return true;
  }
  unsigned char b = (unsigned char)v;
  return mach_bytes(e, &b, 1);
}
static bool mach_rex(nyr_x64_mach_enc_t *e, bool w, bool r, bool x, bool b) {
  unsigned rex = 0x40u | (w ? 0x08u : 0) | (r ? 0x04u : 0) | (x ? 0x02u : 0) |
                 (b ? 0x01u : 0);
  return rex == 0x40u ? true : mach_u8(e, rex);
}

static bool mach_u32(nyr_x64_mach_enc_t *e, uint32_t v) {
  unsigned char b[4] = {(unsigned char)(v & 0xff), (unsigned char)(v >> 8),
                        (unsigned char)(v >> 16), (unsigned char)(v >> 24)};
  return mach_bytes(e, b, 4);
}

static bool mach_i32(nyr_x64_mach_enc_t *e, int32_t v) {
  return mach_u32(e, (uint32_t)v);
}

static bool mach_i64(nyr_x64_mach_enc_t *e, int64_t v) {
  unsigned char b[8];
  for (unsigned i = 0; i < 8; ++i)
    b[i] = (unsigned char)((uint64_t)v >> (i * 8));
  return mach_bytes(e, b, 8);
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
 * Prefix counts of instructions covered by a dedicated loop register, one
 * row of inst_len + 1 entries per color.
 */
typedef struct {
  const size_t *rows;
  size_t inst_len;
} mach_loop_cover_t;

static bool mach_loop_cover_blocks(void *ctx, int color, size_t start,
                                   size_t end) {
  const mach_loop_cover_t *cover = ctx;
  if (color < 0 || color >= MIR_COLOR_N || start > end ||
      end >= cover->inst_len)
    return true;
  const size_t *row = cover->rows + (size_t)color * (cover->inst_len + 1);
  return row[end] - (start ? row[start - 1] : 0) != 0;
}

/*
 * Mark every uncolored segment that is block-local by construction: its vreg
 * is neither live into nor out of the block, the block holds no other
 * segment of it, and the segment opens with its definition. Such a value has
 * no home consumer outside the segment, so any color free over the interval
 * is as good as the one the allocator could not keep.
 */
static size_t mach_inst_input_uses_vreg(const nyr_mach_inst_t *in,
                                        uint32_t vreg);

static void mach_mark_local_spills(const nyr_x64_mach_enc_t *e,
                                   const nyr_mach_func_t *m,
                                   unsigned char *candidate) {
  const nyr_mach_regalloc_t *a = &e->regalloc;
  if (!a->vreg_offsets || a->vreg_len != m->vreg_len)
    return;
  for (size_t i = 0; i < a->segment_len; ++i) {
    const nyr_mach_live_segment_t *s = &a->segments[i];
    if (candidate[i] || s->color >= 0 || s->vreg >= m->vreg_len ||
        s->block >= m->block_len || s->start > s->end ||
        s->end >= m->inst_len ||
        s->spill_reason == NYR_MACH_SPILL_FIXED_CLOBBER ||
        m->vreg_types[s->vreg] != NYR_MACH_TYPE_I64 ||
        (e->loop_gpr && e->loop_gpr[s->vreg] >= 0) ||
        nyr_mach_regalloc_live_in(a, s->block, s->vreg) ||
        nyr_mach_regalloc_live_out(a, s->block, s->vreg))
      continue;
    const nyr_mach_inst_t *def = &m->insts[s->start];
    if (def->dst.kind != NYR_MACH_OPERAND_VREG || def->dst.as.reg != s->vreg ||
        def->opcode == NYR_MACH_STORE || def->opcode == NYR_MACH_CALL ||
        mach_inst_input_uses_vreg(def, s->vreg))
      continue;
    bool unique = true;
    for (size_t k = a->vreg_offsets[s->vreg];
         k < a->vreg_offsets[s->vreg + 1] && unique; ++k)
      unique = k == i || a->segments[k].block != s->block;
    if (unique)
      candidate[i] = 1;
  }
}

/*
 * A literal the encoder rematerializes at each use never reads its color, so
 * holding one across a block only starves the temporaries around it.
 */
static void mach_decolor_literals(nyr_x64_mach_enc_t *e,
                                  const nyr_mach_func_t *m) {
  if (!m->vreg_len)
    return;
  int64_t *value = calloc(m->vreg_len, sizeof(*value));
  bool *known = calloc(m->vreg_len, sizeof(*known));
  if (value && known && mach_build_imm_fold(m, value, known)) {
    /*
     * Remember the decolored literals for the encoder: a definition whose
     * only consumer rematerializes the immediate need not publish it.
     */
    free(e->decolored_literal);
    e->decolored_literal = calloc(m->vreg_len, sizeof(bool));
    if (e->decolored_literal)
      memcpy(e->decolored_literal, known, m->vreg_len * sizeof(bool));
    for (size_t i = 0; i < e->regalloc.segment_len; ++i) {
      nyr_mach_live_segment_t *s = &e->regalloc.segments[i];
      if (s->vreg >= m->vreg_len || !known[s->vreg] || s->color < 0)
        continue;
      s->color = -1;
      s->reload = s->spill = true;
      s->carried = s->acyclic_carried = false;
      if (s->spill_reason != NYR_MACH_SPILL_FIXED_CLOBBER)
        s->spill_reason = NYR_MACH_SPILL_REGISTER_PRESSURE;
    }
  }
  free(value);
  free(known);
}

static void mach_recolor_local_spills(nyr_x64_mach_enc_t *e,
                                      const nyr_mach_func_t *m) {
  if (!e->regalloc.segment_len)
    return;
  unsigned char *candidate = calloc(e->regalloc.segment_len, 1);
  if (!candidate)
    return;
  mach_mark_local_spills(e, m, candidate);
  nyr_mach_regalloc_recolor_locals(m, &e->regalloc, NYR_MACH_REGCLASS_GPR,
                                   candidate, NULL, NULL);
  free(candidate);
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
  for (size_t b = 0; b < m->block_len; ++b) {
    const nyr_mach_block_t *block = &m->blocks[b];
    if (!block->inst_count)
      continue;
    size_t first = block->first_inst;
    size_t last = first + block->inst_count - 1;
    for (size_t v = 0; v < m->vreg_len; ++v) {
      size_t at = v * m->block_len + b;
      if (nyr_mach_regalloc_live_in(&e->regalloc, b, (uint32_t)v))
        ranges->starts[at] = first;
      if (nyr_mach_regalloc_live_out(&e->regalloc, b, (uint32_t)v))
        ranges->ends[at] = last;
    }
    /*
     * Visit operands once instead of rescanning the block for every vreg.
     * Live-in/out still extend the first/last touch to the block edges.
     */
    for (size_t pc = first; pc <= last; ++pc) {
      const nyr_mach_inst_t *in = &m->insts[pc];
      const nyr_mach_operand_t *ops[] = {&in->dst, &in->src0, &in->src1,
                                         &in->src2};
      for (size_t k = 0; k < 4 + in->args_len; ++k) {
        const nyr_mach_operand_t *op = k < 4 ? ops[k] : &in->args[k - 4];
        if (op->kind != NYR_MACH_OPERAND_VREG || op->as.reg >= m->vreg_len)
          continue;
        size_t at = (size_t)op->as.reg * m->block_len + b;
        if (ranges->starts[at] == SIZE_MAX)
          ranges->starts[at] = pc;
        if (pc > ranges->ends[at])
          ranges->ends[at] = pc;
      }
    }
  }
  return true;
}

/*
 * COPY_STRUCT lowers to an INTRINSIC with two pointer operands and an
 * immediate size. Its effects are exact: it reads through src1 and writes
 * through src0, like a load plus a store, so it must not be treated as an
 * opaque memory effect.
 */
static bool mach_is_known_copy(const nyr_mach_inst_t *in) {
  return in->opcode == NYR_MACH_INTRINSIC &&
         in->src0.kind == NYR_MACH_OPERAND_VREG &&
         in->src1.kind == NYR_MACH_OPERAND_VREG &&
         in->src2.kind == NYR_MACH_OPERAND_IMM && in->src2.as.imm > 0;
}

/*
 * Fixed-width block sets for the loop-register proofs. 256 blocks (four
 * 64-bit words); the vector extension gives |, & and ~ per word.
 */
#define MACH_SET_BLOCKS 256u
/*
 * Whole-function home proof cap; see the comment at mach_select_loop_gpr.
 */
#define MACH_LOOP_GPR_MAX_VREGS 640u
typedef uint64_t mach_set_t __attribute__((vector_size(32)));
static inline mach_set_t mach_set_bit(size_t b) {
  mach_set_t s = {0, 0, 0, 0};
  s[b >> 6] = (uint64_t)1 << (b & 63u);
  return s;
}
static inline bool mach_set_any(mach_set_t s) {
  return (s[0] | s[1] | s[2] | s[3]) != 0;
}
static inline bool mach_set_has(mach_set_t s, size_t b) {
  return (s[b >> 6] >> (b & 63u)) & 1u;
}
static inline bool mach_set_eq(mach_set_t a, mach_set_t b) {
  return !mach_set_any(a ^ b);
}
/*
 * Lowest set bit index; the set must be non-empty.
 */
static inline unsigned mach_set_first(mach_set_t s) {
  for (unsigned w = 0; w < 4; ++w)
    if (s[w])
      return w * 64u + (unsigned)__builtin_ctzll(s[w]);
  return 0;
}
static inline mach_set_t mach_set_clear_first(mach_set_t s) {
  for (unsigned w = 0; w < 4; ++w)
    if (s[w]) {
      s[w] &= s[w] - 1;
      break;
    }
  return s;
}
/*
 * Bits 0..n-1 set (n <= 256).
 */
static inline mach_set_t mach_set_prefix(size_t n) {
  mach_set_t s = {0, 0, 0, 0};
  for (unsigned w = 0; w < 4; ++w) {
    size_t lo = (size_t)w * 64u;
    if (n >= lo + 64u)
      s[w] = ~(uint64_t)0;
    else if (n > lo)
      s[w] = ((uint64_t)1 << (n - lo)) - 1u;
  }
  return s;
}

/*
 * A candidate's colour is rejected by interference with an already coloured
 * range. Report which vreg owns it so the deep trace can show whether a hot
 * value lost its colour to a colder one; SIZE_MAX means free and
 * MACH_LOOP_GPR_NO_INTERVAL means the value has no live range at all.
 */
#define MACH_LOOP_GPR_NO_INTERVAL (SIZE_MAX - 1)
static size_t mach_loop_gpr_color_blocker(const mach_loop_gpr_ranges_t *ranges,
                                          const size_t *color_heads,
                                          const size_t *next_colored,
                                          const mach_set_t *present,
                                          uint32_t vreg, int color) {
  bool has_interval = false;
  if (present)
    has_interval = mach_set_any(present[vreg]);
  else
    for (size_t b = 0; b < ranges->block_len; ++b)
      has_interval |=
          ranges->starts[(size_t)vreg * ranges->block_len + b] != SIZE_MAX;
  if (!has_interval)
    return MACH_LOOP_GPR_NO_INTERVAL;
  for (size_t other = color_heads[color]; other != SIZE_MAX;
       other = next_colored[other]) {
    if (present) {
      mach_set_t common = present[vreg] & present[other];
      while (mach_set_any(common)) {
        unsigned b = mach_set_first(common);
        size_t a = (size_t)vreg * ranges->block_len + b;
        size_t c = other * ranges->block_len + b;
        if (ranges->starts[a] <= ranges->ends[c] &&
            ranges->starts[c] <= ranges->ends[a])
          return other;
        common = mach_set_clear_first(common);
      }
    } else {
      for (size_t b = 0; b < ranges->block_len; ++b) {
        size_t a = (size_t)vreg * ranges->block_len + b;
        size_t c = other * ranges->block_len + b;
        if (ranges->starts[a] != SIZE_MAX && ranges->starts[c] != SIZE_MAX &&
            ranges->starts[a] <= ranges->ends[c] &&
            ranges->starts[c] <= ranges->ends[a])
          return other;
      }
    }
  }
  return SIZE_MAX;
}

static bool mach_loop_gpr_color_available(const mach_loop_gpr_ranges_t *ranges,
                                          const size_t *color_heads,
                                          const size_t *next_colored,
                                          const mach_set_t *present,
                                          uint32_t vreg, int color) {
  return mach_loop_gpr_color_blocker(ranges, color_heads, next_colored, present,
                                     vreg, color) == SIZE_MAX;
}
static bool mach_loop_gpr_call_safe(const nyr_mach_func_t *m,
                                    const mach_loop_gpr_ranges_t *ranges,
                                    const unsigned *depth, uint32_t vreg,
                                    bool allow_ordinary_calls) {
  if (!m || !ranges || !depth || vreg >= m->vreg_len)
    return false;
  for (size_t b = 0; b < m->block_len; ++b) {
    size_t at = (size_t)vreg * ranges->block_len + b;
    if (ranges->starts[at] == SIZE_MAX)
      continue;
    for (size_t pc = ranges->starts[at]; pc <= ranges->ends[at]; ++pc) {
      nyr_mach_opcode_t op = m->insts[pc].opcode;
      if (op == NYR_MACH_CALL || op == NYR_MACH_INTRINSIC ||
          op == NYR_MACH_INLINE_ASM) {
        if (allow_ordinary_calls &&
            (op == NYR_MACH_CALL || mach_is_known_copy(&m->insts[pc])))
          continue;
        return false;
      }
    }
  }
  return true;
}

/*
 * R8/R9 are scratch for general div/mod, but not immediate power-of-two
 * lowering.
 * Keep it out of the former live ranges. RSI and RDI are not scratch in scalar
 * or vector lowering; their loop homes still require a call-free range.
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
    for (size_t pc = ranges->starts[at]; pc <= ranges->ends[at]; ++pc) {
      const nyr_mach_inst_t *in = &m->insts[pc];
      if (in->opcode == NYR_MACH_DIV || in->opcode == NYR_MACH_MOD) {
        /*
         * The immediate power-of-two path uses RAX/RDX (and RCX for
         * remainder), not R8. Match the encoder's divisor check exactly;
         * unknown and general divisors still exclude a dedicated R8 home.
         */
        unsigned shift = 0;
        if (!mach_pow2_divisor(&in->src1, NULL, NULL, 0, &shift))
          return false;
      }
    }
  }
  return true;
}

/*
 * A caller-saved color other than RSI/RDI may serve as a dedicated loop home
 * only when no instruction in the value's complete live range clobbers it
 * through a fixed-register lowering (division, remainder, variable shift,
 * reductions) or a call. This is the same per-instruction contract the
 * segment allocator splits on; R8 additionally keeps its exact pow2 proof.
 *
 * Ordinary segments survive encoder-local scratch use because the encoder
 * flushes and reloads them; a dedicated home is assumed resident on every
 * edge, so it must not span any lowering that borrows R9/RCX/RDX. Admit only
 * opcodes whose x64 lowering is known to use no scratch beyond RAX/R10/R11:
 * the fixed-clobber mask then covers immediate DIV/MOD's RCX/RDX use. STORE
 * is excluded because its value scratch may select these registers.
 */
static bool mach_loop_gpr_scratch_free_op(const nyr_mach_inst_t *in) {
  switch (in->opcode) {
  case NYR_MACH_COPY:
  case NYR_MACH_LEA:
  case NYR_MACH_ADD:
  case NYR_MACH_SUB:
  case NYR_MACH_AND:
  case NYR_MACH_OR:
  case NYR_MACH_XOR:
  case NYR_MACH_MUL:
  case NYR_MACH_CMP:
  case NYR_MACH_SELECT:
  case NYR_MACH_BR:
  case NYR_MACH_BR_IF:
  case NYR_MACH_LOAD:
    return true;
  case NYR_MACH_SHL:
  case NYR_MACH_SAR:
  case NYR_MACH_SHR:
  case NYR_MACH_ROR:
  case NYR_MACH_ROR32:
  case NYR_MACH_VSHR:
    return in->src1.kind == NYR_MACH_OPERAND_IMM;
  case NYR_MACH_DIV:
  case NYR_MACH_MOD: {
    unsigned shift = 0;
    return mach_pow2_divisor(&in->src1, NULL, NULL, 0, &shift);
  }
  case NYR_MACH_STORE:
    return in->dst.kind == NYR_MACH_OPERAND_FRAME &&
           in->src0.kind == NYR_MACH_OPERAND_IMM;
  default:
    return false;
  }
}

static bool mach_loop_gpr_fixed_safe(const nyr_mach_func_t *m,
                                     const mach_loop_gpr_ranges_t *ranges,
                                     uint32_t vreg, int color) {
  if (!m || !ranges || vreg >= m->vreg_len || color < 0 || color >= 64)
    return false;
  if (color == 8 && !mach_loop_gpr_r8_safe(m, ranges, vreg))
    return false;
  for (size_t b = 0; b < m->block_len; ++b) {
    size_t at = (size_t)vreg * ranges->block_len + b;
    if (ranges->starts[at] == SIZE_MAX)
      continue;
    for (size_t pc = ranges->starts[at]; pc <= ranges->ends[at]; ++pc) {
      const nyr_mach_inst_t *in = &m->insts[pc];
      if (!mach_loop_gpr_scratch_free_op(in) ||
          (nyr_mach_inst_fixed_clobbers(m, in, NYR_MACH_REGCLASS_GPR) &
           (UINT64_C(1) << color)))
        return false;
    }
  }
  return true;
}

static bool mach_loop_gpr_fixed_cached(const nyr_mach_func_t *m,
                                       const mach_loop_gpr_ranges_t *ranges,
                                       uint32_t vreg, int color,
                                       uint8_t *cache) {
  size_t i = (size_t)color * m->vreg_len + vreg;
  if (!cache[i])
    cache[i] = mach_loop_gpr_fixed_safe(m, ranges, vreg, color) ? 2 : 1;
  return cache[i] == 2;
}

static bool mach_inst_uses_vreg(const nyr_mach_inst_t *in, uint32_t vreg);
static bool mach_vreg_feeds_block_address(const nyr_x64_mach_enc_t *e,
                                          uint32_t vreg);
static bool mach_vector_types_present(const nyr_mach_func_t *mach);
static bool mach_is_float(const nyr_mach_func_t *m,
                          const nyr_mach_operand_t *op);

/*
 * Canonical vreg homes lie below the source frame slots. With no addressable
 * frame or physical-register operands, MIR cannot form or expose an address
 * into that private spill area. Payload stores therefore cannot alias it.
 * Opaque operations are excluded separately by the caller.
 */
static bool mach_spill_homes_are_private(const nyr_mach_func_t *mach) {
  /*
   * Spill homes are private when no frame address can escape. A FRAME
   * operand denotes the slot's value for every opcode (arithmetic sources,
   * call arguments, load/store addressing are all direct [rbp-off] accesses)
   * except LEA, which materializes the slot's address. Fixed-register
   * operands and stack-passed or aggregate parameters keep the conservative
   * answer.
   */
  if (!mach || mach->param_count > 6)
    return false;
  for (size_t p = 0; mach->param_types && p < mach->param_count; ++p)
    if (mach->param_types[p] == NYIR_PARAM_AGGREGATE)
      return false;
  for (size_t i = 0; i < mach->inst_len; ++i) {
    const nyr_mach_inst_t *in = &mach->insts[i];
    const nyr_mach_operand_t *ops[] = {&in->dst, &in->src0, &in->src1,
                                       &in->src2};
    for (size_t k = 0; k < sizeof(ops) / sizeof(ops[0]); ++k) {
      if (ops[k]->kind == NYR_MACH_OPERAND_PREG)
        return false;
      /*
       * A C pointer derived from a local object cannot reach the compiler's
       * spill slots without undefined behavior.
       */
      if (ops[k]->kind == NYR_MACH_OPERAND_FRAME &&
          in->opcode == NYR_MACH_LEA && k > 0 && !mach->c_abi)
        return false;
    }
    for (size_t k = 0; k < in->args_len; ++k)
      if (in->args[k].kind == NYR_MACH_OPERAND_PREG)
        return false;
  }
  return true;
}

/*
 * Indirect stores need canonical homes for values that remain live afterward,
 * since the destination may alias a vreg spill slot. Values whose last use is
 * the store itself (or an earlier instruction) do not need a surviving home;
 * keep those short ranges eligible for ordinary register allocation.
 */
static void mach_mark_operand_live(uint64_t *live, size_t vreg_len,
                                   const nyr_mach_operand_t *op) {
  if (op && op->kind == NYR_MACH_OPERAND_VREG && op->as.reg < vreg_len)
    live[op->as.reg >> 6] |= UINT64_C(1) << (op->as.reg & 63);
}

static bool mach_keep_homes_for_store_live_values(nyr_x64_mach_enc_t *e) {
  if (!e || !e->mach || !e->regalloc.segments || e->mach->vreg_len == 0)
    return false;
  const nyr_mach_func_t *mach = e->mach;
  if (mach->vreg_len > SIZE_MAX - 63)
    return false;
  size_t words = (mach->vreg_len + 63) / 64;
  uint64_t *needs_home = calloc(words, sizeof(*needs_home));
  uint64_t *live = calloc(words, sizeof(*live));
  if (!needs_home || !live) {
    free(needs_home);
    free(live);
    return false;
  }
  for (size_t b = 0; b < mach->block_len; ++b) {
    const nyr_mach_block_t *block = &mach->blocks[b];
    size_t first = block->first_inst;
    size_t end = first + block->inst_count;
    memset(live, 0, words * sizeof(*live));
    for (uint32_t v = 0; v < mach->vreg_len; ++v)
      if (nyr_mach_regalloc_live_out(&e->regalloc, b, v))
        live[v >> 6] |= UINT64_C(1) << (v & 63);
    for (size_t i = end; i > first;) {
      --i;
      const nyr_mach_inst_t *in = &mach->insts[i];
      if ((in->opcode == NYR_MACH_STORE &&
           in->dst.kind == NYR_MACH_OPERAND_VREG) ||
          mach_is_known_copy(in))
        for (size_t w = 0; w < words; ++w)
          needs_home[w] |= live[w];
      if (in->opcode != NYR_MACH_STORE &&
          in->dst.kind == NYR_MACH_OPERAND_VREG &&
          in->dst.as.reg < mach->vreg_len)
        live[in->dst.as.reg >> 6] &= ~(UINT64_C(1) << (in->dst.as.reg & 63));
      mach_mark_operand_live(live, mach->vreg_len, &in->src0);
      mach_mark_operand_live(live, mach->vreg_len, &in->src1);
      mach_mark_operand_live(live, mach->vreg_len, &in->src2);
      for (size_t a = 0; a < in->args_len; ++a)
        mach_mark_operand_live(live, mach->vreg_len, &in->args[a]);
      if (in->opcode == NYR_MACH_STORE)
        mach_mark_operand_live(live, mach->vreg_len, &in->dst);
    }
  }
  for (size_t i = 0; i < e->regalloc.segment_len; ++i) {
    nyr_mach_live_segment_t *seg = &e->regalloc.segments[i];
    if (seg->vreg >= mach->vreg_len ||
        !(needs_home[seg->vreg >> 6] & (UINT64_C(1) << (seg->vreg & 63))) ||
        (e->loop_gpr && e->loop_gpr[seg->vreg] >= 0) || seg->color < 0)
      continue;
    seg->color = -1;
    seg->reload = true;
    seg->spill = true;
    seg->carried = seg->acyclic_carried = false;
    if (seg->spill_reason != NYR_MACH_SPILL_FIXED_CLOBBER)
      seg->spill_reason = NYR_MACH_SPILL_BLOCK_CONFLICT;
  }
  free(needs_home);
  free(live);
  return true;
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
    for (size_t pc = ranges->starts[at]; pc <= ranges->ends[at]; ++pc) {
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

/*
 * A potentially aliasing indirect store can invalidate a stack-home-backed
 * value. When home privacy is unproven, only assign dedicated registers to
 * values whose complete live ranges avoid such stores.
 */
static bool mach_loop_gpr_store_safe(const nyr_mach_func_t *m,
                                     const mach_loop_gpr_ranges_t *ranges,
                                     uint32_t vreg, bool proven_private) {
  if (!m || !ranges || vreg >= m->vreg_len)
    return false;
  if (proven_private)
    return true;
  for (size_t b = 0; b < ranges->block_len; ++b) {
    size_t at = (size_t)vreg * ranges->block_len + b;
    if (ranges->starts[at] == SIZE_MAX)
      continue;
    for (size_t pc = ranges->starts[at]; pc <= ranges->ends[at]; ++pc) {
      const nyr_mach_inst_t *in = &m->insts[pc];
      if (in->opcode == NYR_MACH_STORE && in->dst.kind == NYR_MACH_OPERAND_VREG)
        return false;
    }
  }
  return true;
}

/*
 * Recoloring extends dst's dedicated register back to v's definition at
 * producer_pc. Verify call, scratch, vector, store-alias and interference
 * safety for every recolored operand, not just the original destination.
 */
enum {
  MACH_LOOP_HAZARD_CALL,
  MACH_LOOP_HAZARD_OPAQUE,
  MACH_LOOP_HAZARD_INDIRECT_STORE,
  MACH_LOOP_HAZARD_VECTOR,
  MACH_LOOP_HAZARD_COUNT
};

typedef struct {
  size_t *counts;
  size_t stride;
} mach_loop_hazard_index_t;

static bool mach_loop_hazard_build(const nyr_mach_func_t *m,
                                   mach_loop_hazard_index_t *index) {
  if (!m || !index || m->inst_len == SIZE_MAX ||
      m->inst_len + 1 >
          SIZE_MAX / MACH_LOOP_HAZARD_COUNT / sizeof(*index->counts))
    return false;
  index->stride = m->inst_len + 1;
  index->counts =
      calloc(MACH_LOOP_HAZARD_COUNT * index->stride, sizeof(*index->counts));
  if (!index->counts)
    return false;
  for (size_t pc = 0; pc < m->inst_len; ++pc) {
    const nyr_mach_inst_t *in = &m->insts[pc];
    bool vector = false;
    const nyr_mach_operand_t *ops[] = {&in->dst, &in->src0, &in->src1,
                                       &in->src2};
    for (size_t k = 0; k < 4 && !vector; ++k)
      vector = mach_loop_gpr_operand_is_vector(m, ops[k]);
    for (size_t k = 0; k < in->args_len && !vector; ++k)
      vector = mach_loop_gpr_operand_is_vector(m, &in->args[k]);
    const bool flags[MACH_LOOP_HAZARD_COUNT] = {
        in->opcode == NYR_MACH_CALL || mach_is_known_copy(in),
        (in->opcode == NYR_MACH_INTRINSIC && !mach_is_known_copy(in)) ||
            in->opcode == NYR_MACH_INLINE_ASM,
        (in->opcode == NYR_MACH_STORE &&
         in->dst.kind == NYR_MACH_OPERAND_VREG) ||
            mach_is_known_copy(in),
        vector};
    for (size_t k = 0; k < MACH_LOOP_HAZARD_COUNT; ++k) {
      size_t *row = index->counts + k * index->stride;
      row[pc + 1] = row[pc] + (size_t)flags[k];
    }
  }
  return true;
}

static bool mach_loop_hazard_has(const mach_loop_hazard_index_t *index,
                                 unsigned type, size_t start, size_t end) {
  const size_t *row = index->counts + (size_t)type * index->stride;
  return end >= start && row[end + 1] != row[start];
}

static bool mach_loop_gpr_vector_safe_indexed(
    const nyr_mach_func_t *m, const mach_loop_gpr_ranges_t *ranges,
    uint32_t vreg, const mach_loop_hazard_index_t *index) {
  if (!index->counts)
    return mach_loop_gpr_vector_safe(m, ranges, vreg);
  for (size_t b = 0; b < ranges->block_len; ++b) {
    size_t at = (size_t)vreg * ranges->block_len + b;
    if (ranges->starts[at] != SIZE_MAX &&
        mach_loop_hazard_has(index, MACH_LOOP_HAZARD_VECTOR, ranges->starts[at],
                             ranges->ends[at]))
      return false;
  }
  return true;
}

static bool mach_loop_gpr_store_safe_indexed(
    const nyr_mach_func_t *m, const mach_loop_gpr_ranges_t *ranges,
    uint32_t vreg, bool proven_private, const mach_loop_hazard_index_t *index) {
  if (!index->counts)
    return mach_loop_gpr_store_safe(m, ranges, vreg, proven_private);
  if (proven_private)
    return true;
  for (size_t b = 0; b < ranges->block_len; ++b) {
    size_t at = (size_t)vreg * ranges->block_len + b;
    if (ranges->starts[at] != SIZE_MAX &&
        mach_loop_hazard_has(index, MACH_LOOP_HAZARD_INDIRECT_STORE,
                             ranges->starts[at], ranges->ends[at]))
      return false;
  }
  return true;
}

static bool mach_loop_gpr_call_safe_indexed(
    const nyr_mach_func_t *m, const mach_loop_gpr_ranges_t *ranges,
    const unsigned *depth, uint32_t vreg, bool allow_ordinary_calls,
    const mach_loop_hazard_index_t *index) {
  if (!index->counts)
    return mach_loop_gpr_call_safe(m, ranges, depth, vreg,
                                   allow_ordinary_calls);
  for (size_t b = 0; b < ranges->block_len; ++b) {
    size_t at = (size_t)vreg * ranges->block_len + b;
    if (ranges->starts[at] == SIZE_MAX)
      continue;
    size_t start = ranges->starts[at], end = ranges->ends[at];
    if (mach_loop_hazard_has(index, MACH_LOOP_HAZARD_OPAQUE, start, end) ||
        (!allow_ordinary_calls &&
         mach_loop_hazard_has(index, MACH_LOOP_HAZARD_CALL, start, end)))
      return false;
  }
  return true;
}

static bool mach_recolor_interval_safe(
    const nyr_x64_mach_enc_t *e, const mach_loop_gpr_ranges_t *ranges_in,
    const unsigned *depth, const bool *vector_safe,
    const nyr_mach_block_t *block, size_t end, size_t copy_pc, uint32_t src,
    size_t producer_pc, uint32_t dst, int color, bool proven_private) {
  const nyr_mach_func_t *m = e->mach;
  const mach_loop_gpr_ranges_t ranges = *ranges_in;
  /*
   * Recoloring extends dst's physical home back to src's definition.
   * Its old proof says nothing about this added interval: even a
   * previously colored src may have used a different scratch-safe
   * color. Reuse the candidate proofs over src's complete ranges,
   * including the producer itself and live-in/out block extensions.
   */
  bool safe = mach_loop_gpr_store_safe(m, &ranges, src, proven_private) &&
              mach_loop_gpr_call_safe(m, &ranges, depth, src,
                                      color < MIR_CALLEE_COLOR_N) &&
              (color < 8 || mach_loop_gpr_fixed_safe(m, &ranges, src, color)) &&
              (color < 8 || vector_safe[src]);
  for (size_t pc = producer_pc; safe && pc < copy_pc; ++pc) {
    const nyr_mach_inst_t *in = &m->insts[pc];
    if (pc != producer_pc && mach_inst_uses_vreg(in, dst))
      safe = false;
    if (pc > producer_pc && in->opcode != NYR_MACH_STORE &&
        in->dst.kind == NYR_MACH_OPERAND_VREG && in->dst.as.reg == dst)
      safe = false;
    if (pc > producer_pc &&
        (in->opcode == NYR_MACH_CALL || in->opcode == NYR_MACH_INLINE_ASM ||
         in->opcode == NYR_MACH_INTRINSIC || in->opcode == NYR_MACH_BR ||
         in->opcode == NYR_MACH_BR_IF || in->opcode == NYR_MACH_RET))
      safe = false;
  }
  for (size_t pc = block->first_inst; safe && pc < end; ++pc) {
    const nyr_mach_inst_t *in = &m->insts[pc];
    if (pc == copy_pc || in->source_pc != UINT32_MAX ||
        in->opcode != NYR_MACH_COPY)
      continue;
    if ((in->dst.kind == NYR_MACH_OPERAND_VREG && in->dst.as.reg == dst) ||
        (in->src0.kind == NYR_MACH_OPERAND_VREG && in->src0.as.reg == dst))
      safe = false;
  }
  /*
   * The source is about to share dst's dedicated preg from its
   * definition through the edge copy. Other loop homes may share this
   * color when their ranges are disjoint from dst, so also check the
   * shorter source interval against every such home. The range builder
   * includes operand uses and live-in/out extension for each block.
   */
  for (size_t other = 0; safe && other < m->vreg_len; ++other) {
    if (other == dst || other == src || e->loop_gpr[other] != e->loop_gpr[dst])
      continue;
    for (size_t rb = 0; rb < ranges.block_len; ++rb) {
      size_t source_at = (size_t)src * ranges.block_len + rb;
      size_t other_at = other * ranges.block_len + rb;
      if (ranges.starts[source_at] != SIZE_MAX &&
          ranges.starts[other_at] != SIZE_MAX &&
          ranges.starts[source_at] <= ranges.ends[other_at] &&
          ranges.starts[other_at] <= ranges.ends[source_at]) {
        safe = false;
        break;
      }
    }
  }
  return safe;
}

static void mach_allocate_loop_gprs(nyr_x64_mach_enc_t *e, const char *name,
                                    bool private_spill_homes) {
  const nyr_mach_func_t *m = e->mach;
  /*
   * Keep an escape hatch for benchmarking and diagnosis. Values proven
   * loop-carried by the CFG analysis below may use dedicated homes. A
   * single-use constant divide result is also eligible: its definition uses
   * fixed scratch registers, and the measured binary kernel otherwise spills
   * the quotient before its only loop-local consumer. Restrict this exception
   * to one definition and one use so it cannot extend register lifetimes.
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
   *
   * An indirect store may overwrite a canonical spill home if private-home
   * separation is unproven. Reject only candidates live across such stores;
   * unrelated store-free ranges retain their register-home eligibility.
   * All candidates still require the call, edge and fixed-register proofs.
   */
  if (!m->vreg_len || m->vreg_len > MACH_LOOP_GPR_MAX_VREGS || m->block_len > MACH_SET_BLOCKS ||
      getenv("NYTRIX_FORCE_STACK_GPR"))
    return;
  size_t *score = calloc(m->vreg_len, sizeof(*score));
  unsigned *defs = calloc(m->vreg_len, sizeof(*defs));
  unsigned *uses = calloc(m->vreg_len, sizeof(*uses));
  size_t *def_pc = malloc(m->vreg_len * sizeof(*def_pc));
  bool *other_def = calloc(m->vreg_len, sizeof(*other_def));
  bool *loop_carried = calloc(m->vreg_len, sizeof(*loop_carried));
  bool *vector_safe = calloc(m->vreg_len, sizeof(*vector_safe));
  bool *store_safe = calloc(m->vreg_len, sizeof(*store_safe));
  unsigned *depth = calloc(m->block_len, sizeof(*depth));
  int *colors = malloc(m->vreg_len * sizeof(*colors));
  size_t *next_colored = malloc(m->vreg_len * sizeof(*next_colored));
  bool *call_safe_callee = calloc(m->vreg_len, sizeof(*call_safe_callee));
  bool *call_safe_caller = calloc(m->vreg_len, sizeof(*call_safe_caller));
  uint8_t *fixed_safe_cache =
      calloc((size_t)MIR_COLOR_N * m->vreg_len, sizeof(*fixed_safe_cache));
  size_t color_heads[MIR_COLOR_N];
  for (size_t c = 0; c < MIR_COLOR_N; ++c)
    color_heads[c] = SIZE_MAX;
  int64_t *folded_value = calloc(m->vreg_len, sizeof(*folded_value));
  bool *folded_known = calloc(m->vreg_len, sizeof(*folded_known));
  mach_loop_gpr_ranges_t ranges = {0};
  if (!score || !defs || !uses || !def_pc || !other_def || !loop_carried ||
      !vector_safe || !store_safe || !depth || !colors || !next_colored ||
      !call_safe_callee || !call_safe_caller || !fixed_safe_cache ||
      !folded_value || !folded_known ||
      !mach_build_imm_fold(m, folded_value, folded_known) ||
      !mach_loop_gpr_ranges_build(e, &ranges)) {
    free(score);
    free(defs);
    free(uses);
    free(def_pc);
    free(other_def);
    free(loop_carried);
    free(vector_safe);
    free(store_safe);
    free(depth);
    free(colors);
    free(next_colored);
    free(call_safe_callee);
    free(call_safe_caller);
    free(fixed_safe_cache);
    free(folded_value);
    free(folded_known);
    free(ranges.starts);
    free(ranges.ends);
    return;
  }
  mach_loop_hazard_index_t hazard = {0};
  if (m->inst_len >= 96 && m->vreg_len >= 16 &&
      !getenv("NYTRIX_X64_NO_HAZARD_INDEX"))
    mach_loop_hazard_build(m, &hazard);
  /*
   * mach_set_t is a 32-byte vector and the mask accesses are aligned vector
   * loads/stores. calloc only guarantees 16-byte alignment, and a mask whose
   * address lands on a 16-byte boundary faults on the first OR (movaps).
   * Over-allocate and align the base manually so every element is 32-byte
   * aligned; present_raw is what gets freed.
   */
  uint8_t *present_raw = NULL;
  mach_set_t *present = NULL;
  if (m->block_len >= 8 && m->vreg_len >= 16 &&
      !getenv("NYTRIX_X64_NO_BLOCK_MASK")) {
    present_raw =
        calloc(1, m->vreg_len * sizeof(mach_set_t) + sizeof(mach_set_t));
    if (present_raw)
      present =
          (mach_set_t *)(((uintptr_t)present_raw + (sizeof(mach_set_t) - 1u)) &
                         ~(uintptr_t)(sizeof(mach_set_t) - 1u));
  }
  if (present)
    for (size_t v = 0; v < m->vreg_len; ++v)
      for (size_t b = 0; b < ranges.block_len; ++b)
        if (ranges.starts[v * ranges.block_len + b] != SIZE_MAX)
          present[v] |= mach_set_bit(b);
  for (size_t v = 0; v < m->vreg_len; ++v)
    vector_safe[v] =
        mach_loop_gpr_vector_safe_indexed(m, &ranges, (uint32_t)v, &hazard);
  bool proven_private = private_spill_homes || mach_vector_types_present(m);
  for (size_t v = 0; v < m->vreg_len; ++v)
    store_safe[v] = mach_loop_gpr_store_safe_indexed(m, &ranges, (uint32_t)v,
                                                     proven_private, &hazard);
  for (size_t v = 0; v < m->vreg_len; ++v) {
    colors[v] = -1;
    def_pc[v] = SIZE_MAX;
  }
  /*
   * Build actual CFG edges; block-number intervals include unrelated exits.
   */
  mach_set_t succ[MACH_SET_BLOCKS] = {{0}}, pred[MACH_SET_BLOCKS] = {{0}},
             dom[MACH_SET_BLOCKS] = {{0}};
  mach_set_t header_loops[MACH_SET_BLOCKS] = {{0}};
  const mach_set_t valid_blocks = mach_set_prefix(m->block_len);
  for (size_t b = 0; b < m->block_len; ++b) {
    const nyr_mach_block_t *block = &m->blocks[b];
    bool falls = true;
    for (size_t n = 0; n < block->inst_count; ++n) {
      const nyr_mach_inst_t *in = &m->insts[block->first_inst + n];
      if ((in->opcode == NYR_MACH_BR || in->opcode == NYR_MACH_BR_IF) &&
          in->src1.kind == NYR_MACH_OPERAND_BLOCK &&
          in->src1.as.block_index < m->block_len) {
        succ[b] |= mach_set_bit(in->src1.as.block_index);
        if (in->opcode == NYR_MACH_BR)
          falls = false;
      } else if (in->opcode == NYR_MACH_RET) {
        falls = false;
      }
    }
    if (falls && b + 1 < m->block_len)
      succ[b] |= mach_set_bit(b + 1);
  }
  for (size_t b = 0; b < m->block_len; ++b)
    for (size_t t = 0; t < m->block_len; ++t)
      if (mach_set_has(succ[b], t))
        pred[t] |= mach_set_bit(b);
  mach_set_t reachable = mach_set_bit(0);
  for (;;) {
    mach_set_t next = reachable;
    for (size_t b = 0; b < m->block_len; ++b)
      if (mach_set_has(reachable, b))
        next |= succ[b];
    next &= valid_blocks;
    if (mach_set_eq(next, reachable))
      break;
    reachable = next;
  }
  for (size_t b = 0; b < m->block_len; ++b)
    if (mach_set_has(reachable, b))
      dom[b] = b == 0 ? mach_set_bit(0) : reachable;
  bool dom_changed;
  do {
    dom_changed = false;
    for (size_t b = 1; b < m->block_len; ++b) {
      mach_set_t bit = mach_set_bit(b);
      if (!mach_set_has(reachable, b))
        continue;
      mach_set_t incoming = pred[b] & reachable;
      mach_set_t intersection = reachable;
      if (!mach_set_any(incoming))
        intersection = mach_set_prefix(0);
      else
        for (size_t p = 0; p < m->block_len; ++p)
          if (mach_set_has(incoming, p))
            intersection &= dom[p];
      mach_set_t value = intersection | bit;
      if (!mach_set_eq(value, dom[b])) {
        dom[b] = value;
        dom_changed = true;
      }
    }
  } while (dom_changed);
  for (size_t latch = 0; latch < m->block_len; ++latch) {
    for (size_t header = 0; header <= latch; ++header) {
      mach_set_t hbit = mach_set_bit(header);
      if (!mach_set_has(succ[latch], header) ||
          !mach_set_has(reachable, header) || !mach_set_has(dom[latch], header))
        continue;
      mach_set_t members = hbit | mach_set_bit(latch);
      size_t work[MACH_SET_BLOCKS], work_len = 1;
      work[0] = latch;
      for (size_t wi = 0; wi < work_len; ++wi) {
        size_t block = work[wi];
        if (block == header)
          continue;
        mach_set_t incoming = pred[block] & reachable & ~members;
        for (size_t p = 0; p < m->block_len; ++p)
          if (mach_set_has(incoming, p)) {
            members |= mach_set_bit(p);
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
      if (mach_set_has(header_loops[header], b))
        ++depth[b];
  /*
   * Call-home publication is not yet proven across loop backedges and joins.
   * The per-vreg range proof below excludes values that cross a call, while
   * allowing unrelated call-free loops in the same function to use registers.
   */
  for (size_t b = 0; b < m->block_len; ++b) {
    const nyr_mach_block_t *block = &m->blocks[b];
    size_t weight =
        depth[b] ? (size_t)1 << (depth[b] > 5 ? 15 : depth[b] * 3) : 0;
    for (size_t n = 0; n < block->inst_count; ++n) {
      const nyr_mach_inst_t *in = &m->insts[block->first_inst + n];
      if (in->dst.kind == NYR_MACH_OPERAND_VREG &&
          in->dst.as.reg < m->vreg_len && in->opcode != NYR_MACH_STORE) {
        uint32_t v = in->dst.as.reg;
        ++defs[v];
        def_pc[v] = block->first_inst + n;
        bool loop_home_def =
            (in->opcode == NYR_MACH_COPY &&
             (in->src0.kind == NYR_MACH_OPERAND_VREG ||
              in->src0.kind == NYR_MACH_OPERAND_IMM)) ||
            in->opcode == NYR_MACH_LOAD || in->opcode == NYR_MACH_LEA ||
            ((in->opcode == NYR_MACH_SAR || in->opcode == NYR_MACH_SHR ||
              in->opcode == NYR_MACH_SHL) &&
             (in->src1.kind == NYR_MACH_OPERAND_IMM ||
              (in->src1.kind == NYR_MACH_OPERAND_VREG &&
               in->src1.as.reg < m->vreg_len &&
               folded_known[in->src1.as.reg])) &&
             m->c_abi) ||
            in->opcode == NYR_MACH_ADD || in->opcode == NYR_MACH_SUB ||
            in->opcode == NYR_MACH_AND || in->opcode == NYR_MACH_OR ||
            in->opcode == NYR_MACH_XOR || in->opcode == NYR_MACH_MUL ||
            in->opcode == NYR_MACH_SELECT ||
            (in->opcode == NYR_MACH_DIV &&
             in->src1.kind == NYR_MACH_OPERAND_IMM) ||
            (m->c_abi && in->opcode == NYR_MACH_MOD &&
             (in->src1.kind == NYR_MACH_OPERAND_IMM ||
              (in->src1.kind == NYR_MACH_OPERAND_VREG &&
               in->src1.as.reg < m->vreg_len &&
               folded_known[in->src1.as.reg])));
        other_def[in->dst.as.reg] |= !loop_home_def;
      }
      const nyr_mach_operand_t *ops[] = {&in->src0, &in->src1, &in->src2};
      for (size_t k = 0; k < 3; ++k)
        if (ops[k]->kind == NYR_MACH_OPERAND_VREG &&
            ops[k]->as.reg < m->vreg_len) {
          uint32_t v = ops[k]->as.reg;
          score[v] += weight;
          ++uses[v];
        }
      if (in->opcode == NYR_MACH_STORE &&
          in->dst.kind == NYR_MACH_OPERAND_VREG && in->dst.as.reg < m->vreg_len)
        ++uses[in->dst.as.reg];
      for (size_t ai = 0; in->args && ai < in->args_len; ++ai)
        if (in->args[ai].kind == NYR_MACH_OPERAND_VREG &&
            in->args[ai].as.reg < m->vreg_len)
          ++uses[in->args[ai].as.reg];
    }
  }
  for (uint32_t v = 0; v < m->vreg_len; ++v) {
    call_safe_callee[v] =
        mach_loop_gpr_call_safe_indexed(m, &ranges, depth, v, true, &hazard);
    call_safe_caller[v] =
        mach_loop_gpr_call_safe_indexed(m, &ranges, depth, v, false, &hazard);
  }
  bool reserved[MIR_COLOR_N] = {false};
  size_t selected = 0;
  for (int color = 0; color < MIR_COLOR_N; ++color) {
    int preg = mach_color_preg[color];
    bool callee_saved = color < MIR_CALLEE_COLOR_N;
    /*
     * R8, R9, RCX and RDX are fixed scratch only at instructions the
     * shared fixed-clobber contract names (division, remainder, variable
     * shifts, reductions) and at calls. mach_loop_gpr_fixed_safe proves the
     * former over the whole range; the call proof covers the rest.
     */
    bool r8_caller_saved = color >= 8;
    /*
     * C allocates ordinary values in rsi/rdi first, and the overlap check
     * does not yet see every conflict with a loop register there.
     */
    bool data_caller_saved = !m->c_abi && (color == 5 || color == 6);
    if (!callee_saved && !r8_caller_saved && !data_caller_saved)
      continue;
    if (preg == 11)
      continue;
    for (;;) {
      size_t best = SIZE_MAX, best_score = 0;
      bool best_is_carried = false;
      for (size_t v = 0; v < m->vreg_len; ++v) {
        bool vector_ok = callee_saved || data_caller_saved || vector_safe[v];
        if (ny_dbg_is_enabled(NY_DBG_DEEP) && defs[v] > 0 &&
            m->vreg_types[v] == NYR_MACH_TYPE_I64 &&
            (loop_carried[v] || score[v] >= 16)) {
          bool simple_defs = !other_def[v];
          bool safe_stores = store_safe[v];
          bool unassigned = colors[v] < 0;
          bool score_ok = true;
          bool call_safe =
              callee_saved ? call_safe_callee[v] : call_safe_caller[v];
          bool fixed_safe = callee_saved ||
                            (r8_caller_saved && mach_loop_gpr_fixed_cached(
                                                    m, &ranges, (uint32_t)v,
                                                    color, fixed_safe_cache)) ||
                            data_caller_saved;
          bool is_unfolded = !folded_known[v];
          size_t color_blocker = mach_loop_gpr_color_blocker(
              &ranges, color_heads, next_colored, present, (uint32_t)v, color);
          bool color_available = color_blocker == SIZE_MAX;
          ny_dbg_enter_context(name);
          ny_dbg_heuristic_event(
              "X64_LOOP_HOME",
              "candidate v%zu preg=%d score=%zu carried=%d eligible=%d "
              "defs=%d "
              "simple=%d unassigned=%d unfolded=%d call-safe=%d fixed-safe=%d "
              "vector-safe=%d store-safe=%d available=%d blocker=%ld",
              v, mach_color_preg[color], score[v], (int)loop_carried[v],
              (int)(simple_defs && unassigned && score_ok && call_safe &&
                    fixed_safe && vector_ok && is_unfolded && safe_stores &&
                    color_available),
              1, (int)simple_defs, (int)unassigned, (int)is_unfolded,
              (int)call_safe, (int)fixed_safe, (int)vector_ok, (int)safe_stores,
              (int)color_available,
              color_blocker == SIZE_MAX
                  ? -1L
                  : (color_blocker == MACH_LOOP_GPR_NO_INTERVAL
                         ? -2L
                         : (long)color_blocker));
          ny_dbg_exit_context(name);
        }
        bool fixed_safe = callee_saved ||
                          (r8_caller_saved && mach_loop_gpr_fixed_cached(
                                                  m, &ranges, (uint32_t)v,
                                                  color, fixed_safe_cache)) ||
                          data_caller_saved;
        bool single_use_constant_div =
            defs[v] == 1 && uses[v] == 1 && def_pc[v] < m->inst_len &&
            m->insts[def_pc[v]].opcode == NYR_MACH_DIV &&
            m->insts[def_pc[v]].src1.kind == NYR_MACH_OPERAND_IMM;
        bool candidate_loop_local = loop_carried[v] || score[v] >= 16 ||
                                    (!callee_saved && single_use_constant_div);
        /*
         * Rank by loop-weighted uses, with carry only breaking equal costs.
         * An outer-loop carry must not displace a more frequently used
         * temporary in an inner loop merely because it crosses a backedge.
         * The range, interference, and call proofs apply to both kinds.
         *
         * A dedicated color has the same meaning on every CFG edge. Caller-
         * saved colors require call-free ranges; R8 additionally avoids
         * constant div/mod scratch. Share colors only between disjoint
         * ranges and reserve them from the ordinary allocator.
         */
        if (defs[v] > 0 && candidate_loop_local && !other_def[v] &&
            colors[v] < 0 && m->vreg_types[v] == NYR_MACH_TYPE_I64 &&
            !folded_known[v] && store_safe[v] &&
            (callee_saved ? call_safe_callee[v] : call_safe_caller[v]) &&
            fixed_safe && vector_ok &&
            (best == SIZE_MAX || score[v] > best_score ||
             (score[v] == best_score && loop_carried[v] && !best_is_carried) ||
             (best != SIZE_MAX && score[v] == best_score &&
              loop_carried[v] == best_is_carried && def_pc[v] < def_pc[best])) &&
            mach_loop_gpr_color_available(&ranges, color_heads, next_colored,
                                          present, (uint32_t)v, color)) {
          best = v;
          best_score = score[v];
          best_is_carried = loop_carried[v];
        }
      }
      if (best == SIZE_MAX)
        break;
      colors[best] = color;
      next_colored[best] = color_heads[color];
      color_heads[color] = best;
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
    /*
     * A latch producer with one use can write directly into the selected
     * loop-home register for its PHI destination. The old destination must
     * be dead after the producer and no parallel-copy snapshot may mention
     * it; otherwise the source and destination still need distinct homes.
     * A previously selected temporary may be recolored under the same proof.
     */
    for (size_t b = 0; b < m->block_len; ++b) {
      const nyr_mach_block_t *block = &m->blocks[b];
      size_t end = (size_t)block->first_inst + block->inst_count;
      if (end > m->inst_len)
        continue;
      for (size_t copy_pc = block->first_inst; copy_pc < end; ++copy_pc) {
        const nyr_mach_inst_t *copy = &m->insts[copy_pc];
        if (copy->source_pc != UINT32_MAX || copy->opcode != NYR_MACH_COPY ||
            copy->narrow32 || copy->dst.kind != NYR_MACH_OPERAND_VREG ||
            copy->src0.kind != NYR_MACH_OPERAND_VREG)
          continue;
        uint32_t dst = copy->dst.as.reg, src = copy->src0.as.reg;
        if (dst >= m->vreg_len || src >= m->vreg_len || dst == src ||
            e->loop_gpr[dst] < 0 || e->loop_gpr[src] == e->loop_gpr[dst] ||
            m->vreg_types[dst] != NYR_MACH_TYPE_I64 ||
            m->vreg_types[src] != NYR_MACH_TYPE_I64 || defs[src] != 1 ||
            uses[src] == 0 || def_pc[src] < block->first_inst ||
            def_pc[src] >= copy_pc || other_def[src])
          continue;
        /*
         * Extra consumers are allowed only inside (producer, copy]: they read
         * src from dst's register, which is dead there (checked below). A
         * use after the edge copy, in another block, or a live-out src would
         * observe dst's next value instead.
         */
        if (uses[src] != 1) {
          if (nyr_mach_regalloc_live_out(&e->regalloc, b, src))
            continue;
          size_t in_window = 0;
          for (size_t pc = def_pc[src] + 1; pc <= copy_pc; ++pc)
            in_window += mach_inst_input_uses_vreg(&m->insts[pc], src);
          if (in_window != uses[src])
            continue;
        }
        size_t producer_pc = def_pc[src];
        const nyr_mach_inst_t *producer = &m->insts[producer_pc];
        if (producer->opcode == NYR_MACH_COPY &&
            producer->source_pc == UINT32_MAX)
          continue;
        int color = e->loop_gpr[dst];
        bool safe = mach_recolor_interval_safe(
            e, &ranges, depth, vector_safe, block, end, copy_pc, src,
            producer_pc, dst, color, proven_private);
        /*
         * A source-level copy `src = w` between the real producer and the
         * edge copy would still need a move. When w is single-definition,
         * defined earlier in this block, not live-out, and used only before
         * the edge copy, give w the same home under the same proofs so the
         * whole chain collapses into dst's register.
         */
        uint32_t chain = UINT32_MAX;
        if (safe && producer->opcode == NYR_MACH_COPY &&
            producer->src0.kind == NYR_MACH_OPERAND_VREG &&
            !producer->narrow32) {
          uint32_t w = producer->src0.as.reg;
          if (w < m->vreg_len && w != dst && w != src && defs[w] == 1 &&
              !other_def[w] && m->vreg_types[w] == NYR_MACH_TYPE_I64 &&
              def_pc[w] >= block->first_inst && def_pc[w] < producer_pc &&
              (e->loop_gpr[w] < 0 || e->loop_gpr[w] != color) &&
              !nyr_mach_regalloc_live_out(&e->regalloc, b, w)) {
            size_t in_window = 0;
            for (size_t pc = def_pc[w] + 1; pc <= copy_pc; ++pc)
              in_window += mach_inst_input_uses_vreg(&m->insts[pc], w);
            if (in_window == uses[w] &&
                mach_recolor_interval_safe(e, &ranges, depth, vector_safe,
                                           block, end, copy_pc, w, def_pc[w],
                                           dst, color, proven_private))
              chain = w;
          }
        }
        /*
         * A SELECT producer is encoded as `mov dst, arm; cmov dst, other`.
         * When one arm is single-definition, defined earlier in this block,
         * not live-out, and read only by this SELECT, give it the loop-home
         * register under the same interval proof: the SELECT then selects
         * in place and the move disappears.
         */
        uint32_t select_arm = UINT32_MAX;
        /*
         * The SELECT is the producer itself or the chained copy's source.
         */
        const nyr_mach_inst_t *sel = producer;
        size_t sel_pc = producer_pc;
        if (chain != UINT32_MAX) {
          sel_pc = def_pc[chain];
          sel = &m->insts[sel_pc];
        }
        if (safe && sel->opcode == NYR_MACH_SELECT &&
            !getenv("NYTRIX_NO_SELECT_ARM_HOME")) {
          for (int k = 0; k < 2 && select_arm == UINT32_MAX; ++k) {
            const nyr_mach_operand_t *arm = k ? &sel->src1 : &sel->src2;
            const nyr_mach_operand_t *peer = k ? &sel->src2 : &sel->src1;
            if (arm->kind != NYR_MACH_OPERAND_VREG)
              continue;
            uint32_t w = arm->as.reg;
            if (w >= m->vreg_len || w == dst || w == src || w == chain ||
                defs[w] != 1 || other_def[w] ||
                m->vreg_types[w] != NYR_MACH_TYPE_I64 ||
                def_pc[w] < block->first_inst || def_pc[w] >= sel_pc ||
                (peer->kind == NYR_MACH_OPERAND_VREG && peer->as.reg == w) ||
                (sel->src0.kind == NYR_MACH_OPERAND_VREG &&
                 sel->src0.as.reg == w) ||
                (e->loop_gpr[w] >= 0 && e->loop_gpr[w] == color) ||
                nyr_mach_regalloc_live_out(&e->regalloc, b, w))
              continue;
            size_t in_window = 0;
            for (size_t pc = def_pc[w] + 1; pc <= copy_pc; ++pc)
              in_window += mach_inst_input_uses_vreg(&m->insts[pc], w);
            if (in_window == uses[w] &&
                mach_recolor_interval_safe(e, &ranges, depth, vector_safe,
                                           block, end, copy_pc, w, def_pc[w],
                                           dst, color, proven_private))
              select_arm = w;
          }
        }
        if (!safe)
          continue;
        e->loop_gpr[src] = e->loop_gpr[dst];
        if (chain != UINT32_MAX)
          e->loop_gpr[chain] = e->loop_gpr[dst];
        if (select_arm != UINT32_MAX)
          e->loop_gpr[select_arm] = e->loop_gpr[dst];
        if (ny_dbg_is_enabled(NY_DBG_DEEP)) {
          ny_dbg_enter_context(name);
          ny_dbg_heuristic_event(
              "X64_LOOP_HOME",
              "coalesced latch producer v%u into loop-home v%u preg=%d", src,
              dst, mach_color_preg[e->loop_gpr[dst]]);
          ny_dbg_exit_context(name);
        }
      }
    }
    /*
     * Spill ordinary segments whose interval overlaps a loop home of the
     * same color. A dedicated home only owns its register over its proven
     * live range; elsewhere in the function the color stays available, so
     * reserving it function-wide would spill unrelated straight-line code.
     * Touching intervals count as overlap. Without the coverage table, fall
     * back to spilling every ordinary segment of a reserved color.
     */
    size_t *coverage =
        calloc((size_t)MIR_COLOR_N * (m->inst_len + 1), sizeof(*coverage));
    if (coverage) {
      for (size_t v = 0; v < m->vreg_len; ++v) {
        int c = e->loop_gpr[v];
        if (c < 0 || c >= MIR_COLOR_N)
          continue;
        size_t *row = coverage + (size_t)c * (m->inst_len + 1);
        for (size_t rb = 0; rb < ranges.block_len; ++rb) {
          size_t at = v * ranges.block_len + rb;
          if (ranges.starts[at] == SIZE_MAX || ranges.ends[at] >= m->inst_len)
            continue;
          row[ranges.starts[at]] += 1;
          row[ranges.ends[at] + 1] -= 1; /* wraps; restored by prefix sum */
        }
      }
      for (int c = 0; c < MIR_COLOR_N; ++c) {
        size_t *row = coverage + (size_t)c * (m->inst_len + 1);
        size_t live = 0, seen = 0;
        for (size_t pc = 0; pc <= m->inst_len; ++pc) {
          live += row[pc];
          seen += live != 0;
          row[pc] = seen; /* covered pcs in [0, pc] */
        }
      }
    }
    /*
     * Block-local temporaries evicted below keep no cross-block contract, so
     * they are offered another free color afterwards instead of a stack home.
     */
    unsigned char *evicted_local =
        coverage
            ? calloc(e->regalloc.segment_len ? e->regalloc.segment_len : 1, 1)
            : NULL;
    for (size_t i = 0; i < e->regalloc.segment_len; ++i) {
      nyr_mach_live_segment_t *s = &e->regalloc.segments[i];
      if (s->vreg >= m->vreg_len || e->loop_gpr[s->vreg] >= 0 || s->color < 0 ||
          s->color >= MIR_COLOR_N || !reserved[s->color])
        continue;
      if (coverage && s->start <= s->end && s->end < m->inst_len) {
        const size_t *row = coverage + (size_t)s->color * (m->inst_len + 1);
        size_t covered = row[s->end] - (s->start ? row[s->start - 1] : 0);
        if (covered == 0)
          continue;
      }
      if (evicted_local && !s->reload && !s->spill && !s->carried &&
          !s->acyclic_carried && s->spill_reason == NYR_MACH_SPILL_NONE)
        evicted_local[i] = 1;
      s->color = -1;
      s->reload = true;
      s->spill = true;
      s->carried = s->acyclic_carried = false;
      if (s->spill_reason != NYR_MACH_SPILL_FIXED_CLOBBER)
        s->spill_reason = NYR_MACH_SPILL_REGISTER_PRESSURE;
    }
    if (evicted_local) {
      mach_loop_cover_t cover = {coverage, m->inst_len};
      mach_mark_local_spills(e, m, evicted_local);
      nyr_mach_regalloc_recolor_locals(m, &e->regalloc, NYR_MACH_REGCLASS_GPR,
                                       evicted_local, mach_loop_cover_blocks,
                                       &cover);
      free(evicted_local);
    }
    free(coverage);
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
  free(uses);
  free(def_pc);
  free(other_def);
  free(loop_carried);
  free(vector_safe);
  free(store_safe);
  free(depth);
  free(colors);
  free(next_colored);
  free(call_safe_callee);
  free(call_safe_caller);
  free(fixed_safe_cache);
  free(folded_value);
  free(folded_known);
  free(ranges.starts);
  free(ranges.ends);
  free(hazard.counts);
  free(present_raw);
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

/*
 * Returns true when this home store should be omitted.
 */
static bool mach_home_store_skip(nyr_x64_mach_enc_t *e, int off) {
  if (!e || off >= 0)
    return false;
  size_t index = (size_t)(-(int64_t)off);
  if (index >= e->home_limit)
    return false;
  if (e->home_store_counts) {
    e->home_store_counts[index]++;
    e->home_store_seen = true;
  }
  return e->home_dead && e->home_dead[index];
}

static int mach_vreg_home_off(const nyr_x64_mach_enc_t *e, uint32_t vreg) {
  if (!e || !e->mach || !e->vreg_home_offs || vreg >= e->mach->vreg_len)
    return 0;
  return e->vreg_home_offs[vreg];
}

static int mach_home_off_cmp(const void *lhs, const void *rhs) {
  int a = *(const int *)lhs, b = *(const int *)rhs;
  return a < b ? -1 : a > b;
}

/*
 * Spill-slot coloring lets vregs with disjoint live ranges share one home.
 * The sticky-RAX tag is keyed by home offset, so "RAX mirrors this slot"
 * only implies "RAX holds this vreg" while nobody else owns the slot: a
 * register-resident vreg can be defined without touching its home, leaving
 * the tag describing a dead neighbour.
 */
static void mach_build_home_shared_map(nyr_x64_mach_enc_t *e) {
  if (e->vreg_home_shared || !e->mach || !e->vreg_home_offs)
    return;
  size_t n = e->mach->vreg_len;
  unsigned char *shared = calloc(n ? n : 1, 1);
  int *sorted = malloc((n ? n : 1) * sizeof(*sorted));
  if (!shared || !sorted) {
    free(shared);
    free(sorted);
    return; /* unknown: callers answer conservatively */
  }
  size_t m = 0;
  for (size_t v = 0; v < n; ++v)
    if (e->vreg_home_offs[v])
      sorted[m++] = e->vreg_home_offs[v];
  qsort(sorted, m, sizeof(*sorted), mach_home_off_cmp);
  free(e->shared_home_offs);
  e->shared_home_offs = malloc((m ? m : 1) * sizeof(*e->shared_home_offs));
  e->shared_home_off_len = 0;
  for (size_t v = 0; v < n; ++v) {
    int off = e->vreg_home_offs[v];
    shared[v] = 2;
    if (!off)
      continue;
    int *hit = bsearch(&off, sorted, m, sizeof(*sorted), mach_home_off_cmp);
    if (hit && ((hit > sorted && hit[-1] == off) ||
                (hit + 1 < sorted + m && hit[1] == off))) {
      shared[v] = 1;
      if (e->shared_home_offs &&
          (e->shared_home_off_len == 0 ||
           e->shared_home_offs[e->shared_home_off_len - 1] != off))
        e->shared_home_offs[e->shared_home_off_len++] = off;
    }
  }
  free(sorted);
  e->vreg_home_shared = shared;
}

static bool mach_vreg_home_shared(nyr_x64_mach_enc_t *e, uint32_t vreg) {
  if (!e || !e->mach || !e->vreg_home_offs || vreg >= e->mach->vreg_len)
    return false;
  mach_build_home_shared_map(e);
  if (!e->vreg_home_shared)
    return true; /* unknown: be conservative */
  return e->vreg_home_shared[vreg] == 1;
}

/*
 * Whether a home offset belongs to more than one vreg. Site-local sticky
 * checks that see only an offset (load/store elision, call-argument staging)
 * cannot prove which owner's bits RAX holds, so a shared slot never uses the
 * tag. The vreg-keyed checks above stay as they are; this closes the
 * offset-only hole that made spill-slot reuse unsafe to enable by default.
 */
static bool mach_home_off_shared(nyr_x64_mach_enc_t *e, int off) {
  if (!e || !e->mach || !off)
    return false;
  mach_build_home_shared_map(e);
  if (!e->vreg_home_shared)
    return true; /* unknown: be conservative */
  if (!e->shared_home_offs || !e->shared_home_off_len)
    return false;
  return bsearch(&off, e->shared_home_offs, e->shared_home_off_len,
                 sizeof(*e->shared_home_offs), mach_home_off_cmp) != NULL;
}

static int mach_slot_off(const nyr_x64_mach_enc_t *e,
                         const nyr_mach_operand_t *op) {
  if (!e || !e->mach || !op)
    return 0;
  if (op->kind == NYR_MACH_OPERAND_FRAME) {
    size_t slot = op->as.frame_index;
    if (slot > e->mach->frame_slot_len)
      slot = e->mach->frame_slot_len;
    if (e->frame_slot_offsets)
      return e->frame_slot_offsets[slot];
    size_t off = 0;
    for (size_t i = 0; i < slot; ++i) {
      size_t size = e->mach->frame_slots[i].size;
      size_t align = e->mach->frame_slots[i].align;
      if (size < 8)
        size = 8;
      if (!align)
        align = 1;
      off = (off + align - 1) & ~(align - 1);
      off += size;
    }
    return -((int)(e->cs_slots * 8u + off + 8));
  }
  if (op->kind == NYR_MACH_OPERAND_VREG)
    return mach_vreg_home_off(e, op->as.reg);
  return 0;
}

static bool mach_is_i64(const nyr_mach_func_t *m, const nyr_mach_operand_t *op);
static unsigned mach_convert_loop_fpr_clobbers(const nyr_mach_func_t *m,
                                               const nyr_mach_inst_t *in,
                                               const int *loop_regs);
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
                                    unsigned preg, int resident_preg);
static bool mach_load_rax(nyr_x64_mach_enc_t *e, int off);
static bool mach_load_rax_slot(nyr_x64_mach_enc_t *e, int off, int vreg);
static bool mach_store_rax_slot(nyr_x64_mach_enc_t *e, int off, int vreg);
static bool mach_rax_tag_hit(nyr_x64_mach_enc_t *e, int off, int vreg);
static bool mach_pending_rax_hit(nyr_x64_mach_enc_t *e, int off, int vreg);
static bool mach_load_rax_rsp(nyr_x64_mach_enc_t *e, int off);
static bool mach_load_op_rax(nyr_x64_mach_enc_t *e,
                             const nyr_mach_operand_t *op);
static bool mach_store_rax_rsp(nyr_x64_mach_enc_t *e, int off);
static bool mach_load_op_preg(nyr_x64_mach_enc_t *e,
                              const nyr_mach_operand_t *op, unsigned preg);
static bool mach_store_preg_home(nyr_x64_mach_enc_t *e, int preg, int off);
static bool mach_vreg_can_forward_to_next_rax(nyr_x64_mach_enc_t *e,
                                              uint32_t vreg);
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
/*
 * Functions without scalar float values (SysV, AVX2 vector fast path) also
 * allocate vector colors 6..9 to xmm4..xmm7. xmm0..xmm3 stay free: integer
 * reductions, the AVX1 lane expansion and the home-path shuffle use them as
 * scratch.
 */
#define MVEC_COLOR_WIDE_N 10
#define MVEC_WIDE_PREG_BASE 4

/*
 * vzeroupper (C5 F8 77). Legacy-SSE instructions executed while the upper
 * YMM halves are dirty pay a large state-transition penalty on the host
 * (measured: ~81 ns vs ~6 ns per transition). Emitted only where no 256-bit
 * value is register-resident; low 128-bit lanes (scalar floats, v128) are
 * preserved.
 */
static bool mach_vzeroupper(nyr_x64_mach_enc_t *e) {
  return mach_u8(e, 0xc5) && mach_u8(e, 0xf8) && mach_u8(e, 0x77);
}

static unsigned mach_vec_color_preg(int color) {
  return color < MVEC_COLOR_N
             ? (unsigned)(MVEC_PREG_BASE + color)
             : (unsigned)(MVEC_WIDE_PREG_BASE + (color - MVEC_COLOR_N));
}
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
  if (!seg || seg->color < 0 || (size_t)seg->color >= e->vec_color_n)
    return -1;
  return (int)mach_vec_color_preg(seg->color);
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
 * True when vreg has a register colour (not a spill) at instruction inst.
 */
static bool mach_vreg_colored_at(const nyr_x64_mach_enc_t *e, uint32_t vreg,
                                 size_t inst) {
  if (!e || e->force_stack_reads || vreg >= e->colors_len)
    return false;
  if (e->loop_gpr && e->loop_gpr[vreg] >= 0)
    return true;
  const nyr_mach_live_segment_t *seg =
      nyr_mach_regalloc_segment_at(&e->regalloc, vreg, inst);
  return seg && !seg->spill && seg->color >= 0 && seg->color < MIR_COLOR_N;
}

/*
 * A fused select compare's right operand: an int32 immediate (literal or
 * known-constant vreg) or a vreg in a register at the select.
 */
static bool mach_select_cmp_rhs_imm(const nyr_x64_mach_enc_t *e,
                                    const nyr_mach_operand_t *op,
                                    int64_t *imm) {
  int64_t v = 0;
  if (op->kind == NYR_MACH_OPERAND_IMM)
    v = op->as.imm;
  else if (op->kind == NYR_MACH_OPERAND_VREG && e->imm_fold_known &&
           e->imm_fold && op->as.reg < e->mach->vreg_len &&
           e->imm_fold_known[op->as.reg])
    v = e->imm_fold[op->as.reg];
  else
    return false;
  if (v < INT32_MIN || v > INT32_MAX)
    return false;
  *imm = v;
  return true;
}

static bool mach_select_cmp_rhs_ok(const nyr_x64_mach_enc_t *e,
                                   const nyr_mach_operand_t *op, size_t inst) {
  int64_t imm = 0;
  if (mach_select_cmp_rhs_imm(e, op, &imm))
    return true;
  return op->kind == NYR_MACH_OPERAND_VREG &&
         op->reg_class == NYR_MACH_REGCLASS_GPR &&
         mach_vreg_colored_at(e, op->as.reg, inst);
}

/*
 * A fused select arm is encodable when it has a register color at the select,
 * or when it is a proven int32 constant. A cmov cannot take an immediate, so
 * the emitter materializes that arm in the reserved encoder scratch (R10)
 * before the compare. Demanding a color for a constant arm rejected every
 * `cond ? k : x` clamp, which has no register to color; that cost ten
 * instructions per clamp (setcc, movzx, spill, reload, test, cmov) where
 * cmp plus cmov suffices.
 */
static bool mach_select_arm_ok(const nyr_x64_mach_enc_t *e,
                               const nyr_mach_operand_t *op, size_t inst) {
  int64_t imm = 0;
  if (mach_select_cmp_rhs_imm(e, op, &imm))
    return true;
  return op->kind == NYR_MACH_OPERAND_VREG &&
         mach_vreg_colored_at(e, op->as.reg, inst);
}

/*
 * A compare operand stays readable at the select only if its value is still
 * needed there: it is one of the select's arms, or the block reads it again
 * after the select. A value whose last use is the compare may already share
 * its register with something defined in between.
 */
static size_t mach_block_for_inst(const nyr_x64_mach_enc_t *e, size_t inst);
static bool mach_select_cmp_operand_live(const nyr_x64_mach_enc_t *e,
                                         const nyr_mach_inst_t *select,
                                         uint32_t vreg, size_t si) {
  if (mach_op_is_vreg(&select->src1, vreg) ||
      mach_op_is_vreg(&select->src2, vreg))
    return true;
  const nyr_mach_func_t *m = e->mach;
  for (size_t k = si + 1; k < m->inst_len; ++k) {
    if (mach_block_for_inst(e, k) != mach_block_for_inst(e, si))
      break;
    if (mach_inst_input_uses_vreg(&m->insts[k], vreg) > 0)
      return true;
  }
  return false;
}

/*
 * x86 condition nibble for an integer compare, -1 when unsupported. The
 * inverse condition is the nibble with bit 0 flipped.
 */
static int mach_cmov_cc(nyr_mach_cond_t cond) {
  switch (cond) {
  case NYR_MACH_COND_EQ:
    return 0x4;
  case NYR_MACH_COND_NE:
    return 0x5;
  case NYR_MACH_COND_LT:
    return 0xc;
  case NYR_MACH_COND_LE:
    return 0xe;
  case NYR_MACH_COND_GT:
    return 0xf;
  case NYR_MACH_COND_GE:
    return 0xd;
  case NYR_MACH_COND_B:
    return 0x2;
  case NYR_MACH_COND_AE:
    return 0x3;
  case NYR_MACH_COND_BE:
    return 0x6;
  case NYR_MACH_COND_A:
    return 0x7;
  default:
    return -1;
  }
}

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

/*
 * Build a per-instruction list of values whose segments begin or end there,
 * ascending by value so emission order matches a full scan.
 */
static bool mach_seg_index_build(const nyr_mach_regalloc_t *alloc, size_t n,
                                 bool ends, uint32_t **offsets_out,
                                 uint32_t **vregs_out) {
  if (!alloc)
    return false;
  uint32_t *offsets = calloc(n + 2, sizeof(*offsets));
  uint32_t *vregs = malloc((alloc->segment_len + 1) * sizeof(*vregs));
  if (!offsets || !vregs) {
    free(offsets);
    free(vregs);
    return false;
  }
  for (size_t i = 0; i < alloc->segment_len; ++i) {
    const nyr_mach_live_segment_t *seg = &alloc->segments[i];
    size_t pc = ends ? seg->end : seg->start;
    if (pc < n)
      offsets[pc + 2]++;
  }
  for (size_t pc = 0; pc < n; ++pc)
    offsets[pc + 2] += offsets[pc + 1];
  for (size_t i = 0; i < alloc->segment_len; ++i) {
    const nyr_mach_live_segment_t *seg = &alloc->segments[i];
    size_t pc = ends ? seg->end : seg->start;
    if (pc < n)
      vregs[offsets[pc + 1]++] = seg->vreg;
  }
  for (size_t pc = 0; pc < n; ++pc)
    for (uint32_t i = offsets[pc] + 1; i < offsets[pc + 1]; ++i) {
      uint32_t value = vregs[i], j = i;
      for (; j > offsets[pc] && vregs[j - 1] > value; --j)
        vregs[j] = vregs[j - 1];
      vregs[j] = value;
    }
  *offsets_out = offsets;
  *vregs_out = vregs;
  return true;
}

static bool mach_publish_gpr_home(nyr_x64_mach_enc_t *e, uint32_t v, int preg);

static bool mach_begin_segments(nyr_x64_mach_enc_t *e) {
  if (!e || !e->color_seeded)
    return true;
  if (!e->seg_start_offsets &&
      !mach_seg_index_build(&e->regalloc, e->mach->inst_len, false,
                            &e->seg_start_offsets, &e->seg_start_vregs))
    return mach_err(e, "x64 machine form encode: OOM");
  if (e->cur_inst >= e->mach->inst_len)
    return true;
  for (uint32_t at = e->seg_start_offsets[e->cur_inst];
       at < e->seg_start_offsets[e->cur_inst + 1]; ++at) {
    uint32_t v = e->seg_start_vregs[at];
    if (v >= e->colors_len)
      continue;
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
  const nyr_mach_func_t *m = e->mach;
  if (e->cur_block_valid && e->cur_block < m->block_len) {
    const nyr_mach_block_t *b = &m->blocks[e->cur_block];
    if (inst >= b->first_inst && inst - b->first_inst < b->inst_count)
      return e->cur_block;
  }
  size_t lo = 0, hi = m->block_len;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (m->blocks[mid].first_inst <= inst)
      lo = mid + 1;
    else
      hi = mid;
  }
  if (!lo)
    return SIZE_MAX;
  size_t at = lo - 1;
  const nyr_mach_block_t *b = &m->blocks[at];
  if (inst >= b->first_inst && inst - b->first_inst < b->inst_count)
    return at;
  return SIZE_MAX;
}

static bool mach_end_segments(nyr_x64_mach_enc_t *e) {
  if (!e || !e->color_seeded)
    return true;
  if (!e->seg_end_offsets &&
      !mach_seg_index_build(&e->regalloc, e->mach->inst_len, true,
                            &e->seg_end_offsets, &e->seg_end_vregs))
    return mach_err(e, "x64 machine form encode: OOM");
  if (e->cur_inst >= e->mach->inst_len)
    return true;
  for (uint32_t at = e->seg_end_offsets[e->cur_inst];
       at < e->seg_end_offsets[e->cur_inst + 1]; ++at) {
    uint32_t v = e->seg_end_vregs[at];
    if (v >= e->colors_len)
      continue;
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
      if (!mach_publish_gpr_home(e, v, mach_color_preg[seg->color]))
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
static bool mach_rbp_disp(nyr_x64_mach_enc_t *e, unsigned reg, int off);

static bool mach_remat_addr_preg(nyr_x64_mach_enc_t *e, int preg,
                                 const nyr_mach_operand_t *addr) {
  if (!e || !addr || preg < 0)
    return false;
  if (addr->kind == NYR_MACH_OPERAND_FRAME)
    return mach_rex(e, true, preg >= 8, false, false) && mach_u8(e, 0x8d) &&
           mach_rbp_disp(e, (unsigned)preg, mach_slot_off(e, addr));
  if (addr->kind == NYR_MACH_OPERAND_SYMBOL && addr->as.symbol) {
    unsigned char modrm = (unsigned char)(0x05 | ((preg & 7) << 3));
    return mach_rex(e, true, preg >= 8, false, false) && mach_u8(e, 0x8d) &&
           mach_u8(e, modrm) &&
           mach_add_reloc(e, addr->as.symbol, NY_RELOC_PC32) && mach_i32(e, 0);
  }
  return false;
}

/*
 * Frame ModRM narrowing is opt-out so the disp8 layout can be A/B measured:
 * NYTRIX_NO_FRAME_DISP8=1 restores disp32 for every home access.
 */
static bool mach_no_frame_disp8(void) {
  static int cached = -1;
  if (cached < 0) {
    const char *v = getenv("NYTRIX_NO_FRAME_DISP8");
    cached = v && strcmp(v, "0") != 0 && strcmp(v, "off") != 0;
  }
  return cached != 0;
}

static bool mach_rbp_disp(nyr_x64_mach_enc_t *e, unsigned reg, int off) {
  if (!e->home_store_counts)
    e->frame_ops++;
  unsigned r = (reg & 7) << 3;
  if (!mach_no_frame_disp8() && off >= -128 && off <= 127)
    return mach_u8(e, 0x45 | r) && mach_u8(e, (unsigned char)off);
  return mach_u8(e, 0x85 | r) && mach_i32(e, off);
}

/*
 * If preg not yet proven in this block, reload from dual-written stack home.
 */
static bool mach_seed_colored_preg(nyr_x64_mach_enc_t *e, uint32_t vreg) {
  if (!e || vreg >= e->colors_len)
    return true;

  int preg = mach_vreg_assigned_preg(e, vreg);
  if (preg < 0 || (e->color_seeded && e->color_seeded[vreg]))
    return true;

  int off = mach_vreg_home_off(e, vreg);

  if (e->imm_fold && e->imm_fold_known && e->imm_fold_known[vreg]) {
    int64_t imm = e->imm_fold[vreg];

    if (imm == 0) {
      if ((preg >= 8 && !mach_u8(e, 0x45)) || !mach_u8(e, 0x31) ||
          !mach_u8(e, (unsigned)(0xc0 | ((preg & 7) << 3) | (preg & 7))))
        return false;
    } else if (imm > 0 && (uint64_t)imm <= UINT32_MAX) {
      if ((preg >= 8 && !mach_u8(e, 0x41)) ||
          !mach_u8(e, (unsigned)(0xb8 + (preg & 7))) ||
          !mach_i32(e, (int32_t)(uint32_t)imm))
        return false;
    } else if (imm >= INT32_MIN && imm <= INT32_MAX) {
      if (!mach_rex(e, true, false, false, preg >= 8) || !mach_u8(e, 0xc7) ||
          !mach_u8(e, (unsigned)(0xc0 | (preg & 7))) ||
          !mach_i32(e, (int32_t)imm))
        return false;
    } else {
      if (!mach_rex(e, true, false, false, preg >= 8) ||
          !mach_u8(e, (unsigned)(0xb8 + (preg & 7))) || !mach_i64(e, imm))
        return false;
    }
  } else if (e->addr_fold && e->addr_fold_known && e->addr_fold_known[vreg]) {
    if (!mach_remat_addr_preg(e, preg, &e->addr_fold[vreg]))
      return false;
  } else {
    if (!mach_rex(e, true, preg >= 8, false, false) || !mach_u8(e, 0x8b) ||
        !mach_rbp_disp(e, (unsigned)preg, off))
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
static bool mach_load_home_preg(nyr_x64_mach_enc_t *e, int off, unsigned preg);

static bool mach_mov_preg_imm(nyr_x64_mach_enc_t *e, unsigned preg,
                              int64_t value) {
  if (preg >= 16)
    return false;
  if (value == 0) {
    if (!mach_rex(e, false, preg >= 8, false, preg >= 8) || !mach_u8(e, 0x31) ||
        !mach_u8(e, (unsigned char)(0xc0 | ((preg & 7) << 3) | (preg & 7))))
      return false;
  } else if (value >= 0 && (uint64_t)value <= UINT32_MAX) {
    if ((preg >= 8 && !mach_u8(e, 0x41)) ||
        !mach_u8(e, (unsigned char)(0xb8 + (preg & 7))) ||
        !mach_i32(e, (int32_t)(uint32_t)value))
      return false;
  } else if (value >= INT32_MIN && value <= INT32_MAX) {
    if (!mach_rex(e, true, false, false, preg >= 8) || !mach_u8(e, 0xc7) ||
        !mach_u8(e, (unsigned char)(0xc0 | (preg & 7))) ||
        !mach_i32(e, (int32_t)value))
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
  if (e->last_home_store_end != 0 && e->last_home_store_end == e->code.len &&
      e->last_home_store_off == off && e->last_home_store_preg == preg)
    return true;
  /*
   * A dropped store still records what it would have left behind: the next
   * load forwards from this register instead of reading the slot, and must do
   * so identically in both encodings.
   */
  if (!mach_home_store_skip(e, off)) {
    if (!mach_rex(e, true, preg >= 8, false, false) || !mach_u8(e, 0x89) ||
        !mach_rbp_disp(e, (unsigned)preg, off))
      return false;
    if (!e->home_store_counts)
      e->home_store_ops++;
  }
  e->last_home_store_end = e->code.len;
  e->last_home_store_off = off;
  e->last_home_store_preg = preg;
  return true;
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
 * Map the integer arguments of a normal ABI call to their destination
 * registers. Aggregates are reported rather than modeled because the caller
 * only uses this for the direct-argument fast path, and aggregate staging
 * uses RSI/RDI/RCX as scratch. Their presence disables that fast path.
 */
static void mach_call_gp_arg_dests(const nyr_x64_mach_enc_t *e,
                                   const nyr_mach_inst_t *in,
                                   int dest[NYIR_CALL_MAX_ARGS],
                                   bool *has_aggregate) {
  for (size_t ai = 0; ai < NYIR_CALL_MAX_ARGS; ++ai)
    dest[ai] = -1;
  *has_aggregate = false;
  if (!e || !in || !in->args)
    return;

  static const int gp_sysv[] = {7, 6, 2, 1, 8, 9};
  static const int gp_win[] = {1, 2, 8, 9};
  bool is_win = e->target && e->target->abi == NY_NATIVE_ABI_WIN64;
  const int *gp = is_win ? gp_win : gp_sysv;
  size_t max_gp = is_win ? 4 : 6;
  size_t gp_n = 0, ordinal_n = 0;
  size_t n =
      in->args_len < NYIR_CALL_MAX_ARGS ? in->args_len : NYIR_CALL_MAX_ARGS;
  for (size_t ai = 0; ai < n; ++ai) {
    if (in->arg_sizes && in->arg_sizes[ai] > 0) {
      *has_aggregate = true;
      continue;
    }
    bool is_fp = mach_is_float(e->mach, &in->args[ai]);
    if (is_win) {
      if (!is_fp && ordinal_n < max_gp)
        dest[ai] = gp[ordinal_n];
      ++ordinal_n;
    } else if (!is_fp && gp_n < max_gp) {
      dest[ai] = gp[gp_n++];
    }
  }
}

/*
 * Direct placement of an integer call argument into an ABI register. The
 * caller (gp_direct classification) has already proven the source cannot
 * alias an ABI destination and does not rely on the transient %rax tags;
 * this routine only emits the move. A `resident_preg` is a caller-saved
 * argument whose value is dead after this call and whose home flush was
 * skipped: the register still holds the current value even though the call
 * seed was cleared by the classification's pre-flush state.
 */
static bool mach_load_call_arg_preg(nyr_x64_mach_enc_t *e,
                                    const nyr_mach_operand_t *arg,
                                    unsigned preg, int resident_preg) {
  if (!e || !arg)
    return false;
  if (arg->kind == NYR_MACH_OPERAND_VREG) {
    int src = resident_preg;
    if (src < 0)
      src = mach_vreg_preg(e, arg->as.reg);
    bool seeded =
        resident_preg >= 0 || (e->color_seeded && e->color_seeded[arg->as.reg]);
    if (src >= 0 && seeded) {
      if ((unsigned)src == preg)
        return true;
      return mach_mov_preg_preg(e, preg, (unsigned)src);
    }
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

/*
 * Write a single-definition GPR value to its home at most once. A store made
 * after the definition and inside its block runs on every path that reaches a
 * later use, so the home stays current and re-storing it at each branch or
 * call is pure memory traffic. A call instruction flushes before producing
 * its own result, so a store at a defining call does not count.
 */
static bool mach_publish_gpr_home(nyr_x64_mach_enc_t *e, uint32_t v, int preg) {
  bool tracked = e->gpr_home_stored && e->def_count && e->def_inst &&
                 v < e->mach->vreg_len && e->def_count[v] == 1;
  if (tracked && e->gpr_home_stored[v])
    return true;
  if (!mach_store_preg_home(e, preg, mach_vreg_home_off(e, v)))
    return false;
  if (tracked && e->cur_block_valid) {
    size_t def = e->def_inst[v];
    if (def < e->mach->inst_len &&
        mach_block_for_inst(e, def) == e->cur_block &&
        (def < e->cur_inst ||
         (def == e->cur_inst && e->mach->insts[def].opcode != NYR_MACH_CALL)))
      e->gpr_home_stored[v] = true;
  }
  return true;
}

/*
 * Next index >= from whose flag is set, or len. Residency flag arrays are
 * mostly false and were scanned element by element at every call, branch
 * and block (O(values) each, quadratic on large bodies); memchr scans them
 * with vector loads. Flags are bool, so a set flag is the byte 1.
 */
static inline uint32_t mach_next_set(const bool *flags, uint32_t from,
                                     uint32_t len) {
  if (!flags || from >= len)
    return len;
  const void *hit = memchr(flags + from, 1, len - from);
  return hit ? (uint32_t)((const bool *)hit - flags) : len;
}

static bool mach_flush_colored_mask_to_home(nyr_x64_mach_enc_t *e,
                                            uint32_t color_mask) {
  if (!e || !e->color_seeded)
    return true;
  if (e->force_stack_reads)
    return true;
  bool saved = false;
  for (uint32_t v = mach_next_set(e->color_seeded, 0, e->colors_len);
       v < e->colors_len;
       v = mach_next_set(e->color_seeded, v + 1, e->colors_len)) {
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
    if (!seg) {
      e->color_seeded[v] = false;
      continue;
    }
    int color =
        e->loop_gpr && e->loop_gpr[v] >= 0 && e->loop_gpr[v] < MIR_COLOR_N
            ? e->loop_gpr[v]
            : seg->color;
    if (color < 0 || color >= MIR_COLOR_N) {
      e->color_seeded[v] = false;
      continue;
    }
    if (!(color_mask & (UINT32_C(1) << (unsigned)color)))
      continue;
    if (!mach_publish_gpr_home(e, v, mach_color_preg[color]))
      return false;
    e->color_seeded[v] = false;
    saved = true;
  }
  if (saved)
    mach_rax_invalidate(e);
  return true;
}

static bool mach_flush_colored_to_home(nyr_x64_mach_enc_t *e) {
  return mach_flush_colored_mask_to_home(
      e, (UINT32_C(1) << (unsigned)MIR_COLOR_N) - 1);
}

/*
 * Calls home the caller-saved bank because the callee may destroy it.  The
 * callee-saved bank survives the call by ABI, so only values in that bank
 * that are actual call operands need a current home: argument placement reads
 * those operands from their canonical homes.  Keeping the other callee-saved
 * values resident avoids a store/reload pair per live value at every recursive
 * call.
 */
static uint32_t mach_call_gpr_home_mask(nyr_x64_mach_enc_t *e,
                                        const nyr_mach_inst_t *in) {
  (void)e;
  (void)in;
  uint32_t all_colors = (UINT32_C(1) << (unsigned)MIR_COLOR_N) - UINT32_C(1);
  uint32_t callee_colors =
      (UINT32_C(1) << (unsigned)MIR_CALLEE_COLOR_N) - UINT32_C(1);
  return all_colors & ~callee_colors;
}

static void mach_invalidate_call_clobbered_colors(nyr_x64_mach_enc_t *e) {
  if (!e || !e->color_seeded)
    return;
  for (uint32_t v = mach_next_set(e->color_seeded, 0, e->colors_len);
       v < e->colors_len;
       v = mach_next_set(e->color_seeded, v + 1, e->colors_len)) {
    const nyr_mach_live_segment_t *seg = mach_segment_at(e, v, e->cur_inst);
    if (!seg || seg->color < 0 || seg->color >= MIR_CALLEE_COLOR_N)
      e->color_seeded[v] = false;
  }
}

/*
 * Inline bare helpers use r12-r15 as scratch and save/restore them around
 * their bodies.  A value that the allocator seeded in one of those registers
 * is not live in the register after the helper restores the pre-helper value;
 * keeping the seed would make the next home flush spill the restored scratch
 * value over the value's canonical home.  Force such values to reload from
 * that home instead.
 */
static void mach_invalidate_callee_scratch_colors(nyr_x64_mach_enc_t *e) {
  if (!e || !e->color_seeded)
    return;
  for (uint32_t v = mach_next_set(e->color_seeded, 0, e->colors_len);
       v < e->colors_len;
       v = mach_next_set(e->color_seeded, v + 1, e->colors_len)) {
    const nyr_mach_live_segment_t *seg = mach_segment_at(e, v, e->cur_inst);
    if (!seg || seg->color < 0 || seg->color >= MIR_COLOR_N)
      continue;
    int preg = mach_color_preg[seg->color];
    if (preg >= 12 && preg <= 15)
      e->color_seeded[v] = false;
  }
}

/*
 * Dedicated loop registers bridge canonical homes around calls.
 */
static bool mach_loop_gprs_call_home(nyr_x64_mach_enc_t *e, bool restore,
                                     bool preserve_callee) {
  if (!e || !e->loop_gpr)
    return true;
  const nyr_mach_func_t *m = e->mach;
  size_t pc = e->cur_inst;
  size_t bi = mach_block_for_inst(e, pc);
  if (bi >= m->block_len)
    return true;
  if (m->vreg_len > MACH_LOOP_GPR_MAX_VREGS)
    return mach_err(e, "x64: loop-home count exceeds allocation limit");

  const nyr_mach_block_t *block = &m->blocks[bi];
  size_t first[640], last[640];
  for (size_t v = 0; v < m->vreg_len; ++v) {
    if (e->loop_gpr[v] < 0 || e->loop_gpr[v] >= MIR_CALLEE_COLOR_N) {
      first[v] = SIZE_MAX;
      continue;
    }
    bool in = nyr_mach_regalloc_live_in(&e->regalloc, bi, (uint32_t)v);
    bool out = nyr_mach_regalloc_live_out(&e->regalloc, bi, (uint32_t)v);
    first[v] = in ? block->first_inst : SIZE_MAX;
    last[v] = out ? block->first_inst + block->inst_count - 1 : 0;
  }
  for (size_t i = 0; i < e->regalloc.segment_len; ++i) {
    const nyr_mach_live_segment_t *seg = &e->regalloc.segments[i];
    if (seg->block != bi || seg->vreg >= m->vreg_len ||
        e->loop_gpr[seg->vreg] < 0 ||
        e->loop_gpr[seg->vreg] >= MIR_CALLEE_COLOR_N)
      continue;
    size_t v = seg->vreg;
    if (seg->start < first[v])
      first[v] = seg->start;
    if (seg->end > last[v])
      last[v] = seg->end;
  }
  const nyr_mach_inst_t *call = &m->insts[pc];
  for (size_t v = 0; v < m->vreg_len; ++v) {
    int color = e->loop_gpr[v];
    if (color < 0 || color >= MIR_CALLEE_COLOR_N || first[v] > pc ||
        (call->dst.kind == NYR_MACH_OPERAND_VREG && call->dst.as.reg == v))
      continue;
    bool live_out = nyr_mach_regalloc_live_out(&e->regalloc, bi, (uint32_t)v);
    bool live_after = last[v] > pc || live_out;
    bool used_as_arg = false;
    for (size_t ai = 0; ai < call->args_len; ++ai)
      if (call->args[ai].kind == NYR_MACH_OPERAND_VREG &&
          call->args[ai].as.reg == v) {
        used_as_arg = true;
        break;
      }
    if (!used_as_arg && call->src0.kind == NYR_MACH_OPERAND_VREG &&
        call->src0.as.reg == v)
      used_as_arg = true;
    if (preserve_callee) {
      /*
       * Callee-saved loop registers survive the call by ABI, and argument
       * placement can move a seeded loop register directly into its ABI
       * destination.  Leave every loop value resident; an unseeded value
       * still has its canonical home for argument placement to read.
       */
      continue;
    }
    if (restore ? !live_after : !(live_after || used_as_arg))
      continue;
    int preg = mach_color_preg[color];
    int off = mach_vreg_home_off(e, (uint32_t)v);
    if (restore) {
      if (!mach_load_home_preg(e, off, (unsigned)preg))
        return false;
    } else if (!mach_store_preg_home(e, preg, off)) {
      return false;
    }
    if (e->color_seeded)
      e->color_seeded[v] = restore;
  }
  return true;
}

/*
 * Spill only colored values whose physical registers a fixed-register
 * sequence will overwrite.  Reciprocal division owns R8-R10; flushing every
 * color there would turn a cheap loop operation into a full-function spill
 * barrier.
 */
static bool mach_flush_pregs_to_home_impl(nyr_x64_mach_enc_t *e,
                                          unsigned preg_mask,
                                          uint32_t dead_use_vreg,
                                          bool discard_dead_use) {
  if (!e || !e->color_seeded || e->force_stack_reads)
    return true;
  bool saved = false;
  for (uint32_t v = mach_next_set(e->color_seeded, 0, e->colors_len);
       v < e->colors_len;
       v = mach_next_set(e->color_seeded, v + 1, e->colors_len)) {
    if (v == dead_use_vreg) {
      if (discard_dead_use)
        e->color_seeded[v] = false;
      continue;
    }
    const nyr_mach_live_segment_t *seg = mach_segment_at(e, v, e->cur_inst);
    if (!seg || seg->color < 0) {
      e->color_seeded[v] = false;
      continue;
    }
    int preg = mach_color_preg[seg->color];
    if (preg < 0 || preg >= (int)(sizeof(preg_mask) * 8) ||
        !(preg_mask & (1u << preg)))
      continue;
    if (!mach_publish_gpr_home(e, v, preg))
      return false;
    e->color_seeded[v] = false;
    saved = true;
  }
  if (saved)
    mach_rax_invalidate(e);
  return true;
}

static bool mach_flush_pregs_to_home(nyr_x64_mach_enc_t *e,
                                     unsigned preg_mask) {
  return mach_flush_pregs_to_home_impl(e, preg_mask, UINT32_MAX, false);
}

static uint32_t
mach_dead_vreg_use_at_current_inst(const nyr_x64_mach_enc_t *e,
                                   const nyr_mach_operand_t *operand) {
  if (!e || !e->mach || !operand || operand->kind != NYR_MACH_OPERAND_VREG ||
      operand->as.reg >= e->mach->vreg_len)
    return UINT32_MAX;
  const nyr_mach_live_segment_t *seg =
      mach_segment_at(e, operand->as.reg, e->cur_inst);
  if (!seg || seg->end != e->cur_inst ||
      mach_segment_at(e, operand->as.reg, e->cur_inst + 1) ||
      mach_vreg_used_in_later_block(e, operand->as.reg))
    return UINT32_MAX;
  return operand->as.reg;
}

static bool mach_flush_pregs_to_home_preserving_dead_use(
    nyr_x64_mach_enc_t *e, unsigned preg_mask, uint32_t dead_use_vreg,
    bool discard_dead_use) {
  return mach_flush_pregs_to_home_impl(e, preg_mask, dead_use_vreg,
                                       discard_dead_use);
}

static bool mach_vreg_requires_home_after_def(nyr_x64_mach_enc_t *e,
                                              uint32_t vreg) {
  /*
   * Publish the canonical spill home after every definition that may feed a
   * later block. Loop-resident registers survive ordinary CFG edges, but not
   * every generated edge (notably cloned/alternate paths) proves that the
   * register alone is a valid home for all future consumers.
   *
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
   *
   * The reserved register is authoritative only while its seed survives.
   * A dedicated loop value that feeds a memory address must still publish
   * its canonical home: the SIB rematerializer and the base-only fallback
   * load each non-resident address operand from that home, and a re-seed
   * after a call flush reloads it as well. susan's stencil store indexed
   * with a never-written loop-GPR home because this exemption returned
   * before the address feed was consulted.
   */
  bool address_feed = e && mach_vreg_feeds_block_address(e, vreg);
  if (e && e->loop_gpr && vreg < e->colors_len && e->loop_gpr[vreg] >= 0 &&
      !address_feed)
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
  if (mach_vreg_future_use_requires_home(e, vreg))
    return true;
  return mach_vreg_same_block_future_use_requires_home(e, vreg) ||
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
    if (!home_is_current &&
        !(off == mach_vreg_home_off(e, dst->as.reg)
              ? mach_publish_gpr_home(e, dst->as.reg, (int)preg)
              : mach_store_preg_home(e, (int)preg, off)))
      return false;
  }
  /*
   * A load from the value's own home (an aliased parameter slot) leaves the
   * home current for every later use of a single-definition value.
   */
  if (home_is_current && e->gpr_home_stored && e->def_count &&
      dst->as.reg < e->mach->vreg_len && e->def_count[dst->as.reg] == 1 &&
      off == mach_vreg_home_off(e, dst->as.reg))
    e->gpr_home_stored[dst->as.reg] = true;
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
    e->pending_rax_vreg = (int)dst->as.reg;
    return true;
  }
  return mach_store_rax_slot(e, off, dst ? (int)dst->as.reg : -1);
}

static bool mach_mov_reg_rax(nyr_x64_mach_enc_t *e, int preg) {
  if (preg == 0)
    return true;
  if (preg == 10)
    mach_r10_index_invalidate(e);
  return mach_rex(e, true, false, false, preg >= 8) && mach_u8(e, 0x89) &&
         mach_u8(e, (unsigned char)(0xc0 | (preg & 7)));
}

static bool mach_mov_preg_preg(nyr_x64_mach_enc_t *e, int dst, int src) {
  if (dst == src)
    return true;
  if (!mach_rex(e, true, src >= 8, false, dst >= 8) || !mach_u8(e, 0x89) ||
      !mach_u8(e, (unsigned char)(0xc0 | ((src & 7) << 3) | (dst & 7))))
    return false;
  /*
   * R10 is also the cached indexed-address scratch. Generic register copies
   * can overwrite it, so invalidate that cache just as the immediate and
   * RAX-copy helpers do.
   */
  if (dst == 10)
    mach_r10_index_invalidate(e);
  return true;
}

static bool mach_alu_preg_imm(nyr_x64_mach_enc_t *e, nyr_mach_opcode_t op,
                              int dst, int64_t imm) {
  if ((int64_t)(int32_t)imm != imm)
    return false;
  if ((op == NYR_MACH_ADD || op == NYR_MACH_SUB || op == NYR_MACH_OR ||
       op == NYR_MACH_XOR) &&
      imm == 0)
    return true;
  if (op == NYR_MACH_AND && imm == -1)
    return true;
  if (op == NYR_MACH_MUL) {
    if (imm == 1)
      return true;
    if (imm == 0)
      return mach_mov_preg_imm(e, (unsigned)dst, 0);
    if (imm == -1) {
      if (dst == 10)
        mach_r10_index_invalidate(e);
      return mach_rex(e, true, false, false, dst >= 8) && mach_u8(e, 0xf7) &&
             mach_u8(e, (unsigned char)(0xd8 | (dst & 7)));
    }
    if (imm > 0 && (imm & (imm - 1)) == 0) {
      unsigned shift = (unsigned)__builtin_ctzll((uint64_t)imm);
      if (dst == 10)
        mach_r10_index_invalidate(e);
      return mach_rex(e, true, false, false, dst >= 8) &&
             mach_u8(e, shift == 1 ? 0xd1 : 0xc1) &&
             mach_u8(e, (unsigned char)(0xe0 | (dst & 7))) &&
             (shift == 1 || mach_u8(e, (unsigned char)shift));
    }
    unsigned scale = imm == 3 ? 1 : imm == 5 ? 2 : imm == 9 ? 3 : 0;
    if (scale) {
      unsigned low = (unsigned)dst & 7u;
      if (dst == 10)
        mach_r10_index_invalidate(e);
      return mach_rex(e, true, dst >= 8, dst >= 8, dst >= 8) &&
             mach_u8(e, 0x8d) &&
             mach_u8(e, (low == 5 ? 0x44 : 0x04) | (low << 3)) &&
             mach_u8(e, (scale << 6) | (low << 3) | low) &&
             (low != 5 || mach_u8(e, 0));
    }
    bool short_imm = imm >= -128 && imm <= 127;
    if (dst == 10)
      mach_r10_index_invalidate(e);
    return mach_rex(e, true, dst >= 8, false, dst >= 8) &&
           mach_u8(e, short_imm ? 0x6b : 0x69) &&
           mach_u8(e, (unsigned char)(0xc0 | ((dst & 7) << 3) | (dst & 7))) &&
           (short_imm ? mach_u8(e, (unsigned char)imm)
                      : mach_i32(e, (int32_t)imm));
  }
  unsigned ext = op == NYR_MACH_ADD   ? 0
                 : op == NYR_MACH_SUB ? 5
                 : op == NYR_MACH_AND ? 4
                 : op == NYR_MACH_OR  ? 1
                 : op == NYR_MACH_XOR ? 6
                                      : 0xff;
  if (ext == 0xff)
    return false;
  bool short_imm = imm >= -128 && imm <= 127;
  if (dst == 10)
    mach_r10_index_invalidate(e);
  return mach_rex(e, true, false, false, dst >= 8) &&
         mach_u8(e, short_imm ? 0x83 : 0x81) &&
         mach_u8(e, (unsigned char)(0xc0 | (ext << 3) | (dst & 7))) &&
         (short_imm ? mach_u8(e, (unsigned char)imm)
                    : mach_i32(e, (int32_t)imm));
}

/*
 * Emit dst = src * imm directly for the small LEA-friendly odd constants.
 */
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

static bool mach_alu_preg_home(nyr_x64_mach_enc_t *e, nyr_mach_opcode_t op,
                               int dst, int off) {
  unsigned opc = op == NYR_MACH_ADD   ? 0x03
                 : op == NYR_MACH_SUB ? 0x2b
                 : op == NYR_MACH_AND ? 0x23
                 : op == NYR_MACH_OR  ? 0x0b
                 : op == NYR_MACH_XOR ? 0x33
                                      : 0xaf;
  bool emitted = mach_rex(e, true, dst >= 8, false, false) &&
                 (op != NYR_MACH_MUL || mach_u8(e, 0x0f)) &&
                 mach_u8(e, opc) && mach_rbp_disp(e, (unsigned)dst, off);
  if (emitted && !e->home_store_counts)
    e->home_fold_ops++;
  return emitted;
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
    if (mach_rax_tag_hit(e, off, (int)op->as.reg) ||
        mach_pending_rax_hit(e, off, (int)op->as.reg))
      return mach_mov_preg_preg(e, (int)preg, 0);
    if (!mach_seed_colored_preg(e, op->as.reg))
      return false;
    int sp = mach_vreg_preg(e, op->as.reg);
    if (sp >= 0)
      return mach_mov_preg_preg(e, (int)preg, sp);
    /*
     * Load the home directly; staging through RAX adds a move.
     */
    if (preg != 0 && off < 0)
      return mach_load_home_preg(e, off, preg);
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
  unsigned ext = op == NYR_MACH_SHL   ? 4
                 : op == NYR_MACH_SAR ? 7
                 : op == NYR_MACH_SHR ? 5
                 : op == NYR_MACH_ROR ? 1
                                      : 0xff;
  if (ext == 0xff || imm < 0 || imm > 63)
    return false;
  if (imm == 0)
    return true;
  return mach_rex(e, true, false, false, dst >= 8) &&
         mach_u8(e, imm == 1 ? 0xd1 : 0xc1) &&
         mach_u8(e, 0xc0 | (ext << 3) | (dst & 7)) &&
         (imm == 1 || mach_u8(e, (unsigned char)imm));
}

static bool mach_mov_rax_reg(nyr_x64_mach_enc_t *e, int preg) {
  if (preg == 0) {
    mach_rax_invalidate(e);
    return true;
  }
  if (!mach_rex(e, true, false, false, preg >= 8) || !mach_u8(e, 0x8b) ||
      !mach_u8(e, 0xc0 | (preg & 7)))
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

/*
 * Does the sticky tag prove that RAX holds the value at `off`? The tag names
 * an offset, which is proof only while one value owns the slot; a pooled slot
 * reuses it only when the tag names the same vreg. `vreg` < 0 means the site
 * knows no vreg and must require an unshared slot.
 */
static bool mach_rax_tag_hit(nyr_x64_mach_enc_t *e, int off, int vreg) {
  if (e->no_sticky || !e->last_rax_valid || e->last_rax_off != off)
    return false;
  if (vreg >= 0 && e->last_rax_vreg == vreg)
    return true;
  return !mach_home_off_shared(e, off);
}

static bool mach_pending_rax_hit(nyr_x64_mach_enc_t *e, int off, int vreg) {
  if (!e->pending_rax_valid || e->pending_rax_off != off)
    return false;
  if (vreg >= 0 && e->pending_rax_vreg == vreg)
    return true;
  return vreg < 0 && e->pending_rax_vreg < 0 &&
         !mach_home_off_shared(e, off);
}

static bool mach_load_rax(nyr_x64_mach_enc_t *e, int off) {
  /*
   * Skip reload when %rax already holds this slot (QBE-style copy elision).
   */
  if (mach_rax_tag_hit(e, off, -1) || mach_pending_rax_hit(e, off, -1))
    return true;
  if (!(mach_rex(e, true, false, false, false) && mach_u8(e, 0x8b) &&
        mach_rbp_disp(e, 0, off)))
    return false;
  e->last_rax_valid = true;
  e->last_rax_off = off;
  e->last_rax_vreg = -1;
  e->last_rax_sync_end = e->code.len;
  return true;
}

/*
 * Load a vreg's home through RAX. The vreg lets a pooled slot reuse the tag
 * for the same owner instead of reloading on every use.
 */
static bool mach_load_rax_slot(nyr_x64_mach_enc_t *e, int off, int vreg) {
  if (mach_rax_tag_hit(e, off, vreg) || mach_pending_rax_hit(e, off, vreg))
    return true;
  if (!(mach_rex(e, true, false, false, false) && mach_u8(e, 0x8b) &&
        mach_rbp_disp(e, 0, off)))
    return false;
  e->last_rax_valid = true;
  e->last_rax_off = off;
  e->last_rax_vreg = vreg;
  e->last_rax_sync_end = e->code.len;
  return true;
}

static bool mach_store_rax(nyr_x64_mach_enc_t *e, int off) {
  return mach_store_rax_slot(e, off, -1);
}

static bool mach_store_rax_slot(nyr_x64_mach_enc_t *e, int off, int vreg) {
  /*
   * A matching sticky-home tag means RAX still contains the value currently
   * stored at this exact stack slot.  Re-publishing it is a redundant
   * load/store pair; retain the tag and omit the store. An instruction whose
   * destination is also its input has changed RAX since the tag was set.
   */
  if (off != e->aliased_dst_home && e->last_rax_sync_end == e->code.len &&
      (mach_rax_tag_hit(e, off, vreg) || mach_pending_rax_hit(e, off, vreg)))
    return true;
  if (!mach_home_store_skip(e, off) &&
      (!mach_rex(e, true, false, false, false) || !mach_u8(e, 0x89) ||
       !mach_rbp_disp(e, 0, off)))
    return false;
  e->last_rax_valid = true;
  e->last_rax_off = off;
  e->last_rax_vreg = vreg;
  e->last_rax_sync_end = e->code.len;
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
      if (mach_rax_tag_hit(e, off, (int)op->as.reg) ||
          mach_pending_rax_hit(e, off, (int)op->as.reg))
        return true;
      if (!mach_mov_rax_reg(e, preg))
        return false;
      /*
       * Dual-write keeps stack == preg; mark sticky to this home.
       */
      e->last_rax_valid = true;
      e->last_rax_off = off;
      e->last_rax_vreg = (int)op->as.reg;
      return true;
    }
  }
  /*
   * A shared slot's offset alone does not identify its current value.
   */
  int off = mach_slot_off(e, op);
  if (op->kind == NYR_MACH_OPERAND_VREG && e->last_rax_valid &&
      e->last_rax_off == off && mach_vreg_home_shared(e, op->as.reg))
    mach_rax_invalidate(e);
  return mach_load_rax_slot(e, off,
                            op->kind == NYR_MACH_OPERAND_VREG
                                ? (int)op->as.reg
                                : -1);
}

static bool mach_patch_rel8(nyr_x64_mach_enc_t *e, size_t disp_off,
                            size_t target_off) {
  if (!e || disp_off >= e->code.len || target_off > e->code.len)
    return false;
  int64_t delta = (int64_t)target_off - (int64_t)(disp_off + 1);
  if (delta < INT8_MIN || delta > INT8_MAX)
    return mach_err(e, "x64 machine form encode: local branch is out of range");
  e->code.data[disp_off] = (unsigned char)(int8_t)delta;
  return true;
}

static bool mach_patch_rel32(nyr_x64_mach_enc_t *e, size_t disp_off,
                             size_t target_off) {
  if (!e || disp_off > e->code.len || e->code.len - disp_off < 4 ||
      target_off > e->code.len)
    return false;
  int64_t delta = (int64_t)target_off - (int64_t)(disp_off + 4);
  if (delta < INT32_MIN || delta > INT32_MAX)
    return mach_err(e, "x64 machine form encode: local branch is out of range");
  uint32_t value = (uint32_t)(int32_t)delta;
  for (unsigned byte = 0; byte < 4; ++byte)
    e->code.data[disp_off + byte] = (unsigned char)(value >> (byte * 8));
  return true;
}

static bool mach_bare_call_finish(nyr_x64_mach_enc_t *e,
                                  const nyr_mach_inst_t *in);

static bool mach_bare_tbuf_new(nyr_x64_mach_enc_t *e,
                               const nyr_mach_inst_t *in) {
  if (!e || !in || in->args_len != 2)
    return mach_err(e,
                    "x64 bare typed-buffer constructor expects two arguments");
  size_t local_branch[3], fail_branch[3];
  size_t fail_count = 0;
  if (!mach_u8(e, 0x41) || !mach_u8(e, 0x54) || /* push r12 */
      !mach_u8(e, 0x41) || !mach_u8(e, 0x55) || /* push r13 */
      !mach_u8(e, 0x41) || !mach_u8(e, 0x56) || /* push r14 */
      !mach_load_call_arg_rax(e, &in->args[0]) || !mach_mov_reg_rax(e, 12) ||
      !mach_load_call_arg_rax(e, &in->args[1]) || !mach_mov_reg_rax(e, 13) ||
      !mach_u8(e, 0x4d) || !mach_u8(e, 0x85) || !mach_u8(e, 0xe4) ||
      !mach_u8(e, 0x79))
    return false;
  local_branch[0] = e->code.len;
  if (!mach_u8(e, 0) || !mach_u8(e, 0x45) || !mach_u8(e, 0x31) ||
      !mach_u8(e, 0xe4))
    return false;
  if (!mach_patch_rel8(e, local_branch[0], e->code.len) || !mach_u8(e, 0x4d) ||
      !mach_u8(e, 0x85) || !mach_u8(e, 0xed) || !mach_u8(e, 0x7f))
    return false;
  local_branch[1] = e->code.len;
  if (!mach_u8(e, 0) || !mach_mov_preg_imm(e, 13, 1) ||
      !mach_patch_rel8(e, local_branch[1], e->code.len) ||
      !mach_mov_preg_preg(e, 14, 12) || !mach_u8(e, 0x49) ||
      !mach_u8(e, 0x83) || !mach_u8(e, 0xfe) || !mach_u8(e, 8) ||
      !mach_u8(e, 0x7d))
    return false;
  local_branch[2] = e->code.len;
  if (!mach_u8(e, 0) || !mach_mov_preg_imm(e, 14, 8) ||
      !mach_patch_rel8(e, local_branch[2], e->code.len) ||
      !mach_mov_preg_preg(e, 0, 14) || !mach_u8(e, 0x49) || !mach_u8(e, 0xf7) ||
      !mach_u8(e, 0xe5) || !mach_u8(e, 0x48) || !mach_u8(e, 0x85) ||
      !mach_u8(e, 0xd2) || !mach_u8(e, 0x75))
    return false;
  fail_branch[fail_count++] = e->code.len;
  if (!mach_u8(e, 0) || !mach_u8(e, 0x48) || !mach_u8(e, 0x83) ||
      !mach_u8(e, 0xc0) || !mach_u8(e, 0x20) || !mach_u8(e, 0x72))
    return false;
  fail_branch[fail_count++] = e->code.len;
  if (!mach_u8(e, 0) || !mach_mov_preg_preg(e, 6, 0) ||
      !mach_mov_preg_imm(e, 7, 0) || !mach_mov_preg_imm(e, 2, 3) ||
      !mach_mov_preg_imm(e, 10, 34) || !mach_mov_preg_imm(e, 8, -1) ||
      !mach_mov_preg_imm(e, 9, 0) ||
      !mach_mov_preg_imm(e, 0, e->target->syscalls.mmap_nr) ||
      !mach_u8(e, 0x0f) || !mach_u8(e, 0x05) || !mach_u8(e, 0x48) ||
      !mach_u8(e, 0x85) || !mach_u8(e, 0xc0) || !mach_u8(e, 0x78))
    return false;
  fail_branch[fail_count++] = e->code.len;
  if (!mach_u8(e, 0) || !mach_mov_preg_preg(e, 11, 0) ||
      !mach_mov_preg_imm(e, 10, INT64_C(0x4e59544255464d47)) ||
      !mach_u8(e, 0x4d) || !mach_u8(e, 0x89) || !mach_u8(e, 0x13) ||
      !mach_u8(e, 0x4d) || !mach_u8(e, 0x89) || !mach_u8(e, 0x63) ||
      !mach_u8(e, 8) || !mach_u8(e, 0x4d) || !mach_u8(e, 0x89) ||
      !mach_u8(e, 0x6b) || !mach_u8(e, 16) || !mach_u8(e, 0x4d) ||
      !mach_u8(e, 0x89) || !mach_u8(e, 0x73) || !mach_u8(e, 24) ||
      !mach_u8(e, 0x49) || !mach_u8(e, 0x8d) || !mach_u8(e, 0x43) ||
      !mach_u8(e, 32) || !mach_u8(e, 0xeb))
    return false;
  size_t success_branch = e->code.len;
  if (!mach_u8(e, 0))
    return false;
  size_t fail_off = e->code.len;
  if (!mach_u8(e, 0x31) || !mach_u8(e, 0xc0))
    return false;
  size_t finish_off = e->code.len;
  if (!mach_u8(e, 0x41) || !mach_u8(e, 0x5e) || /* pop r14 */
      !mach_u8(e, 0x41) || !mach_u8(e, 0x5d) || /* pop r13 */
      !mach_u8(e, 0x41) || !mach_u8(e, 0x5c))
    return false;
  for (size_t i = 0; i < fail_count; ++i)
    if (!mach_patch_rel8(e, fail_branch[i], fail_off))
      return false;
  if (!mach_patch_rel8(e, success_branch, finish_off))
    return false;
  mach_invalidate_callee_scratch_colors(e);
  mach_invalidate_call_clobbered_colors(e);
  if (!mach_loop_gprs_call_home(e, true, false))
    return false;
  mach_rax_invalidate(e);
  mach_r10_index_invalidate(e);
  if (in->dst.kind != NYR_MACH_OPERAND_NONE &&
      !mach_commit_vreg(e, &in->dst, mach_slot_off(e, &in->dst)))
    return false;
  return true;
}

static bool mach_bare_tbuf_len(nyr_x64_mach_enc_t *e,
                               const nyr_mach_inst_t *in) {
  if (!e || !in || in->args_len != 1)
    return mach_err(e, "x64 bare typed-buffer length expects one argument");
  if (!mach_load_call_arg_rax(e, &in->args[0]) || !mach_u8(e, 0x48) ||
      !mach_u8(e, 0x85) || !mach_u8(e, 0xc0) || !mach_u8(e, 0x74))
    return false;
  size_t null_branch = e->code.len;
  if (!mach_u8(e, 6) || !mach_u8(e, 0x48) || !mach_u8(e, 0x8b) ||
      !mach_u8(e, 0x40) || !mach_u8(e, 0xe8) || !mach_u8(e, 0xeb))
    return false;
  size_t done_branch = e->code.len;
  if (!mach_u8(e, 2))
    return false;
  size_t null_result = e->code.len;
  if (!mach_u8(e, 0x31) || !mach_u8(e, 0xc0))
    return false;
  size_t done = e->code.len;
  if (!mach_patch_rel8(e, null_branch, null_result) ||
      !mach_patch_rel8(e, done_branch, done))
    return false;
  mach_invalidate_call_clobbered_colors(e);
  if (!mach_loop_gprs_call_home(e, true, false))
    return false;
  mach_rax_invalidate(e);
  if (in->dst.kind != NYR_MACH_OPERAND_NONE &&
      !mach_commit_vreg(e, &in->dst, mach_slot_off(e, &in->dst)))
    return false;
  return true;
}

static bool mach_bare_tbuf_index_read(nyr_x64_mach_enc_t *e,
                                      const nyr_mach_inst_t *in) {
  if (!e || !in || in->args_len != 2)
    return mach_err(e, "x64 bare typed-buffer indexing expects two arguments");
  size_t fail_branches[9], fail_count = 0;
  size_t normalize_branch, success_branch;
  if (!mach_u8(e, 0x41) || !mach_u8(e, 0x54) || /* push r12 */
      !mach_u8(e, 0x41) || !mach_u8(e, 0x55) || /* push r13 */
      !mach_u8(e, 0x41) || !mach_u8(e, 0x56) || /* push r14 */
      !mach_u8(e, 0x41) || !mach_u8(e, 0x57) || /* push r15 */
      !mach_load_call_arg_rax(e, &in->args[0]) || !mach_mov_reg_rax(e, 12) ||
      !mach_load_call_arg_rax(e, &in->args[1]) || !mach_mov_reg_rax(e, 13) ||
      !mach_u8(e, 0x4d) || !mach_u8(e, 0x85) || !mach_u8(e, 0xe4) ||
      !mach_u8(e, 0x0f) || !mach_u8(e, 0x84))
    return false;
  fail_branches[fail_count++] = e->code.len;
  if (!mach_i32(e, 0) ||
      /*
       * lea r14,[r12-32]; compare the allocation header magic.
       */
      !mach_u8(e, 0x4d) || !mach_u8(e, 0x8d) || !mach_u8(e, 0x74) ||
      !mach_u8(e, 0x24) || !mach_u8(e, 0xe0) ||
      !mach_mov_preg_imm(e, 10, INT64_C(0x4e59544255464d47)) ||
      !mach_u8(e, 0x4d) || !mach_u8(e, 0x39) || !mach_u8(e, 0x16) ||
      !mach_u8(e, 0x0f) || !mach_u8(e, 0x85))
    return false;
  fail_branches[fail_count++] = e->code.len;
  if (!mach_i32(e, 0) ||
      /*
       * Require a positive count and an eight-byte element stride.
       */
      !mach_u8(e, 0x4d) || !mach_u8(e, 0x8b) || !mach_u8(e, 0x7e) ||
      !mach_u8(e, 0x08) || !mach_u8(e, 0x4d) || !mach_u8(e, 0x85) ||
      !mach_u8(e, 0xff) || !mach_u8(e, 0x0f) || !mach_u8(e, 0x8e))
    return false;
  fail_branches[fail_count++] = e->code.len;
  if (!mach_i32(e, 0) || !mach_u8(e, 0x4d) || !mach_u8(e, 0x8b) ||
      !mach_u8(e, 0x56) || !mach_u8(e, 0x10) || !mach_u8(e, 0x49) ||
      !mach_u8(e, 0x83) || !mach_u8(e, 0xfa) || !mach_u8(e, 8) ||
      !mach_u8(e, 0x0f) || !mach_u8(e, 0x85))
    return false;
  fail_branches[fail_count++] = e->code.len;
  if (!mach_i32(e, 0) ||
      /*
       * Capacity must cover the live count.
       */
      !mach_u8(e, 0x4d) || !mach_u8(e, 0x8b) || !mach_u8(e, 0x56) ||
      !mach_u8(e, 0x18) || !mach_u8(e, 0x4d) || !mach_u8(e, 0x39) ||
      !mach_u8(e, 0xfa) || !mach_u8(e, 0x0f) || !mach_u8(e, 0x8c))
    return false;
  fail_branches[fail_count++] = e->code.len;
  if (!mach_i32(e, 0) ||
      /*
       * Normalize negative indexes against the signed live count.
       */
      !mach_u8(e, 0x4d) || !mach_u8(e, 0x85) || !mach_u8(e, 0xed) ||
      !mach_u8(e, 0x0f) || !mach_u8(e, 0x89))
    return false;
  normalize_branch = e->code.len;
  if (!mach_i32(e, 0) || !mach_u8(e, 0x4d) || !mach_u8(e, 0x01) ||
      !mach_u8(e, 0xfd))
    return false;
  size_t normalized = e->code.len;
  if (!mach_patch_rel32(e, normalize_branch, normalized) || !mach_u8(e, 0x4d) ||
      !mach_u8(e, 0x85) || !mach_u8(e, 0xed) || !mach_u8(e, 0x0f) ||
      !mach_u8(e, 0x8c))
    return false;
  fail_branches[fail_count++] = e->code.len;
  if (!mach_i32(e, 0) || !mach_u8(e, 0x4d) || !mach_u8(e, 0x39) ||
      !mach_u8(e, 0xfd) || !mach_u8(e, 0x0f) || !mach_u8(e, 0x8d))
    return false;
  fail_branches[fail_count++] = e->code.len;
  if (!mach_i32(e, 0) ||
      /*
       * rax = data[index]; leave the failure path process-terminating.
       */
      !mach_u8(e, 0x4b) || !mach_u8(e, 0x8b) || !mach_u8(e, 0x04) ||
      !mach_u8(e, 0xec) || !mach_u8(e, 0xe9))
    return false;
  success_branch = e->code.len;
  if (!mach_i32(e, 0))
    return false;
  size_t failure = e->code.len;
  if (!mach_mov_preg_imm(e, 0, e->target->syscalls.exit_nr) ||
      !mach_mov_preg_imm(e, 7, 1) || !mach_u8(e, 0x0f) || !mach_u8(e, 0x05))
    return false;
  size_t success = e->code.len;
  if (!mach_u8(e, 0x41) || !mach_u8(e, 0x5f) || !mach_u8(e, 0x41) ||
      !mach_u8(e, 0x5e) || !mach_u8(e, 0x41) || !mach_u8(e, 0x5d) ||
      !mach_u8(e, 0x41) || !mach_u8(e, 0x5c))
    return false;
  for (size_t i = 0; i < fail_count; ++i)
    if (!mach_patch_rel32(e, fail_branches[i], failure))
      return false;
  if (!mach_patch_rel32(e, success_branch, success))
    return false;
  return mach_bare_call_finish(e, in);
}

static bool mach_bare_tbuf_pop(nyr_x64_mach_enc_t *e,
                               const nyr_mach_inst_t *in) {
  if (!e || !in || in->args_len != 1)
    return mach_err(e, "x64 bare typed-buffer pop expects one argument");
  size_t zero_branches[3], zero_count = 0;
  size_t unsupported_branches[2], unsupported_count = 0;
  size_t success_branch;
  if (!mach_u8(e, 0x41) || !mach_u8(e, 0x54) || /* push r12 */
      !mach_u8(e, 0x41) || !mach_u8(e, 0x55) || /* push r13 */
      !mach_u8(e, 0x41) || !mach_u8(e, 0x56) || /* push r14 */
      !mach_load_call_arg_rax(e, &in->args[0]) || !mach_mov_reg_rax(e, 12) ||
      !mach_u8(e, 0x4d) || !mach_u8(e, 0x85) || !mach_u8(e, 0xe4) ||
      !mach_u8(e, 0x0f) || !mach_u8(e, 0x84))
    return false;
  zero_branches[zero_count++] = e->code.len;
  if (!mach_i32(e, 0) || !mach_u8(e, 0x4d) || !mach_u8(e, 0x8d) ||
      !mach_u8(e, 0x74) || !mach_u8(e, 0x24) || !mach_u8(e, 0xe0) ||
      !mach_mov_preg_imm(e, 10, INT64_C(0x4e59544255464d47)) ||
      !mach_u8(e, 0x4d) || !mach_u8(e, 0x39) || !mach_u8(e, 0x16) ||
      !mach_u8(e, 0x0f) || !mach_u8(e, 0x85))
    return false;
  zero_branches[zero_count++] = e->code.len;
  if (!mach_i32(e, 0) || !mach_u8(e, 0x4d) || !mach_u8(e, 0x8b) ||
      !mach_u8(e, 0x6e) || !mach_u8(e, 0x08) || !mach_u8(e, 0x4d) ||
      !mach_u8(e, 0x85) || !mach_u8(e, 0xed) || !mach_u8(e, 0x0f) ||
      !mach_u8(e, 0x8e))
    return false;
  zero_branches[zero_count++] = e->code.len;
  if (!mach_i32(e, 0) || !mach_u8(e, 0x4d) || !mach_u8(e, 0x8b) ||
      !mach_u8(e, 0x56) || !mach_u8(e, 0x10) || !mach_u8(e, 0x49) ||
      !mach_u8(e, 0x83) || !mach_u8(e, 0xfa) || !mach_u8(e, 8) ||
      !mach_u8(e, 0x0f) || !mach_u8(e, 0x85))
    return false;
  unsupported_branches[unsupported_count++] = e->code.len;
  if (!mach_i32(e, 0) || !mach_u8(e, 0x4d) || !mach_u8(e, 0x8b) ||
      !mach_u8(e, 0x56) || !mach_u8(e, 0x18) || !mach_u8(e, 0x4d) ||
      !mach_u8(e, 0x39) || !mach_u8(e, 0xea) || !mach_u8(e, 0x0f) ||
      !mach_u8(e, 0x8c))
    return false;
  unsupported_branches[unsupported_count++] = e->code.len;
  if (!mach_i32(e, 0) || !mach_u8(e, 0x49) || !mach_u8(e, 0xff) ||
      !mach_u8(e, 0xcd) || !mach_u8(e, 0x4d) || !mach_u8(e, 0x89) ||
      !mach_u8(e, 0x6e) || !mach_u8(e, 0x08) || !mach_u8(e, 0x4b) ||
      !mach_u8(e, 0x8b) || !mach_u8(e, 0x04) || !mach_u8(e, 0xec) ||
      !mach_u8(e, 0x4b) || !mach_u8(e, 0xc7) || !mach_u8(e, 0x04) ||
      !mach_u8(e, 0xec) || !mach_i32(e, 0) || !mach_u8(e, 0xeb))
    return false;
  success_branch = e->code.len;
  if (!mach_u8(e, 0))
    return false;
  size_t zero = e->code.len;
  if (!mach_u8(e, 0x31) || !mach_u8(e, 0xc0) || !mach_u8(e, 0xe9))
    return false;
  size_t zero_done_branch = e->code.len;
  if (!mach_i32(e, 0))
    return false;
  size_t unsupported = e->code.len;
  if (!mach_mov_preg_imm(e, 0, e->target->syscalls.exit_nr) ||
      !mach_mov_preg_imm(e, 7, 1) || !mach_u8(e, 0x0f) || !mach_u8(e, 0x05))
    return false;
  size_t finish = e->code.len;
  if (!mach_u8(e, 0x41) || !mach_u8(e, 0x5e) || !mach_u8(e, 0x41) ||
      !mach_u8(e, 0x5d) || !mach_u8(e, 0x41) || !mach_u8(e, 0x5c))
    return false;
  for (size_t i = 0; i < zero_count; ++i)
    if (!mach_patch_rel32(e, zero_branches[i], zero))
      return false;
  for (size_t i = 0; i < unsupported_count; ++i)
    if (!mach_patch_rel32(e, unsupported_branches[i], unsupported))
      return false;
  if (!mach_patch_rel8(e, success_branch, finish) ||
      !mach_patch_rel32(e, zero_done_branch, finish))
    return false;
  return mach_bare_call_finish(e, in);
}

static bool mach_bare_bounds_trap(nyr_x64_mach_enc_t *e,
                                  const nyr_mach_inst_t *in) {
  if (!e || !in || in->args_len != 0)
    return mach_err(e, "x64 bare bounds trap expects no arguments");
  if (!mach_mov_preg_imm(e, 0, e->target->syscalls.exit_nr) ||
      !mach_mov_preg_imm(e, 7, 1) || !mach_u8(e, 0x0f) || !mach_u8(e, 0x05))
    return false;
  mach_invalidate_call_clobbered_colors(e);
  return mach_loop_gprs_call_home(e, true, false);
}

static bool mach_bare_call_finish(nyr_x64_mach_enc_t *e,
                                  const nyr_mach_inst_t *in) {
  mach_invalidate_callee_scratch_colors(e);
  mach_invalidate_call_clobbered_colors(e);
  if (!mach_loop_gprs_call_home(e, true, false))
    return false;
  mach_rax_invalidate(e);
  return in->dst.kind == NYR_MACH_OPERAND_NONE ||
         mach_commit_vreg(e, &in->dst, mach_slot_off(e, &in->dst));
}

static bool mach_bare_cstr_concat(nyr_x64_mach_enc_t *e,
                                  const nyr_mach_inst_t *in);

static bool mach_bare_cstr_to_string(nyr_x64_mach_enc_t *e,
                                     const nyr_mach_inst_t *in) {
  if (!e || !in || in->args_len != 1)
    return mach_err(e, "x64 bare C-string bridge expects one argument");
  /*
   * Match the owned-string contract: callers may release the input buffer.
   */
  nyr_mach_operand_t args[2] = {
      in->args[0],
      {.kind = NYR_MACH_OPERAND_IMM, .as.imm = 0},
  };
  nyr_mach_inst_t concat = *in;
  concat.args = args;
  concat.args_len = 2;
  return mach_bare_cstr_concat(e, &concat);
}

static bool mach_bare_print_cstr(nyr_x64_mach_enc_t *e,
                                 const nyr_mach_inst_t *in, int fd) {
  if (!e || !in || in->args_len != 1)
    return mach_err(e, "x64 bare string print expects one argument");
  if (!mach_load_call_arg_rax(e, &in->args[0]) || !mach_mov_reg_rax(e, 6) ||
      !mach_u8(e, 0x31) || !mach_u8(e, 0xd2))
    return false;
  size_t loop = e->code.len;
  if (!mach_u8(e, 0x48) || !mach_u8(e, 0x80) || !mach_u8(e, 0x3c) ||
      !mach_u8(e, 0x16) || !mach_u8(e, 0) || !mach_u8(e, 0x74))
    return false;
  size_t done_branch = e->code.len;
  if (!mach_u8(e, 0) || !mach_u8(e, 0x48) || !mach_u8(e, 0xff) ||
      !mach_u8(e, 0xc2) || !mach_u8(e, 0xeb))
    return false;
  size_t loop_branch = e->code.len;
  if (!mach_u8(e, 0))
    return false;
  size_t write = e->code.len;
  if (!mach_mov_preg_imm(e, 0, e->target->syscalls.write_nr) ||
      !mach_mov_preg_imm(e, 7, fd) || !mach_u8(e, 0x0f) || !mach_u8(e, 0x05) ||
      !mach_u8(e, 0x31) || !mach_u8(e, 0xc0))
    return false;
  if (!mach_patch_rel8(e, done_branch, write) ||
      !mach_patch_rel8(e, loop_branch, loop))
    return false;
  return mach_bare_call_finish(e, in);
}

static bool mach_bare_cstr_equal(nyr_x64_mach_enc_t *e,
                                 const nyr_mach_inst_t *in) {
  if (!e || !in || in->args_len != 2)
    return mach_err(e, "x64 bare C-string equality expects two arguments");
  if (!mach_u8(e, 0x41) || !mach_u8(e, 0x54) || !mach_u8(e, 0x41) ||
      !mach_u8(e, 0x55) || !mach_load_call_arg_rax(e, &in->args[0]) ||
      !mach_mov_reg_rax(e, 12) || !mach_load_call_arg_rax(e, &in->args[1]) ||
      !mach_mov_reg_rax(e, 13) || !mach_u8(e, 0x4d) || !mach_u8(e, 0x39) ||
      !mach_u8(e, 0xec) || !mach_u8(e, 0x74))
    return false;
  size_t equal_pointer_branch = e->code.len;
  if (!mach_u8(e, 0) || !mach_u8(e, 0x4d) || !mach_u8(e, 0x85) ||
      !mach_u8(e, 0xe4) || !mach_u8(e, 0x74))
    return false;
  size_t null_left_branch = e->code.len;
  if (!mach_u8(e, 0) || !mach_u8(e, 0x4d) || !mach_u8(e, 0x85) ||
      !mach_u8(e, 0xed) || !mach_u8(e, 0x74))
    return false;
  size_t null_right_branch = e->code.len;
  if (!mach_u8(e, 0))
    return false;
  size_t loop = e->code.len;
  if (!mach_u8(e, 0x45) || !mach_u8(e, 0x0f) || !mach_u8(e, 0xb6) ||
      !mach_u8(e, 0x14) || !mach_u8(e, 0x24) || !mach_u8(e, 0x45) ||
      !mach_u8(e, 0x0f) || !mach_u8(e, 0xb6) || !mach_u8(e, 0x5d) ||
      !mach_u8(e, 0) || !mach_u8(e, 0x45) || !mach_u8(e, 0x38) ||
      !mach_u8(e, 0xda) || !mach_u8(e, 0x75))
    return false;
  size_t unequal_byte_branch = e->code.len;
  if (!mach_u8(e, 0) || !mach_u8(e, 0x45) || !mach_u8(e, 0x84) ||
      !mach_u8(e, 0xd2) || !mach_u8(e, 0x74))
    return false;
  size_t equal_end_branch = e->code.len;
  if (!mach_u8(e, 0) || !mach_u8(e, 0x49) || !mach_u8(e, 0xff) ||
      !mach_u8(e, 0xc4) || !mach_u8(e, 0x49) || !mach_u8(e, 0xff) ||
      !mach_u8(e, 0xc5) || !mach_u8(e, 0xeb))
    return false;
  size_t loop_branch = e->code.len;
  if (!mach_u8(e, 0))
    return false;
  size_t equal = e->code.len;
  if (!mach_u8(e, 0xb8) || !mach_u32(e, 1) || !mach_u8(e, 0xeb))
    return false;
  size_t done_branch = e->code.len;
  if (!mach_u8(e, 0))
    return false;
  size_t unequal = e->code.len;
  if (!mach_u8(e, 0x31) || !mach_u8(e, 0xc0))
    return false;
  size_t done = e->code.len;
  if (!mach_patch_rel8(e, equal_pointer_branch, equal) ||
      !mach_patch_rel8(e, null_left_branch, unequal) ||
      !mach_patch_rel8(e, null_right_branch, unequal) ||
      !mach_patch_rel8(e, unequal_byte_branch, unequal) ||
      !mach_patch_rel8(e, equal_end_branch, equal) ||
      !mach_patch_rel8(e, loop_branch, loop) ||
      !mach_patch_rel8(e, done_branch, done) || !mach_u8(e, 0x41) ||
      !mach_u8(e, 0x5d) || !mach_u8(e, 0x41) || !mach_u8(e, 0x5c))
    return false;
  return mach_bare_call_finish(e, in);
}

static bool mach_store_rax_rsp(nyr_x64_mach_enc_t *e, int off);
static bool mach_load_rax_rsp(nyr_x64_mach_enc_t *e, int off);

static bool mach_bare_cstr_compare(nyr_x64_mach_enc_t *e,
                                   const nyr_mach_inst_t *in) {
  if (!e || !in || in->args_len != 2)
    return mach_err(e, "x64 bare C-string comparison expects two arguments");
  if (!mach_u8(e, 0x41) || !mach_u8(e, 0x54) || !mach_u8(e, 0x41) ||
      !mach_u8(e, 0x55) || !mach_u8(e, 0x48) || !mach_u8(e, 0x83) ||
      !mach_u8(e, 0xec) || !mach_u8(e, 0x10) ||
      !mach_load_call_arg_rax(e, &in->args[0]) || !mach_store_rax_rsp(e, 0) ||
      !mach_load_call_arg_rax(e, &in->args[1]) || !mach_store_rax_rsp(e, 8) ||
      !mach_load_rax_rsp(e, 0) || !mach_mov_reg_rax(e, 12) ||
      !mach_load_rax_rsp(e, 8) || !mach_mov_reg_rax(e, 13) ||
      !mach_u8(e, 0x4d) || !mach_u8(e, 0x39) || !mach_u8(e, 0xec) ||
      !mach_u8(e, 0x74))
    return false;
  size_t same_pointer_branch = e->code.len;
  if (!mach_u8(e, 0) || !mach_u8(e, 0x4d) || !mach_u8(e, 0x85) ||
      !mach_u8(e, 0xe4) || !mach_u8(e, 0x75))
    return false;
  size_t left_nonnull_branch = e->code.len;
  if (!mach_u8(e, 0) || !mach_u8(e, 0x48) || !mach_u8(e, 0xc7) ||
      !mach_u8(e, 0xc0) || !mach_u32(e, -1) || !mach_u8(e, 0xeb))
    return false;
  size_t null_left_branch = e->code.len;
  size_t right_null_check = e->code.len + 1;
  if (!mach_u8(e, 0) || !mach_u8(e, 0x4d) || !mach_u8(e, 0x85) ||
      !mach_u8(e, 0xed) || !mach_u8(e, 0x75))
    return false;
  size_t right_nonnull_branch = e->code.len;
  if (!mach_u8(e, 0) || !mach_u8(e, 0xb8) || !mach_u32(e, 1) ||
      !mach_u8(e, 0xeb))
    return false;
  size_t null_right_branch = e->code.len;
  if (!mach_u8(e, 0))
    return false;
  size_t loop = e->code.len;
  if (!mach_u8(e, 0x45) || !mach_u8(e, 0x0f) || !mach_u8(e, 0xb6) ||
      !mach_u8(e, 0x14) || !mach_u8(e, 0x24) || !mach_u8(e, 0x45) ||
      !mach_u8(e, 0x0f) || !mach_u8(e, 0xb6) || !mach_u8(e, 0x5d) ||
      !mach_u8(e, 0) || !mach_u8(e, 0x45) || !mach_u8(e, 0x39) ||
      !mach_u8(e, 0xda) || !mach_u8(e, 0x75))
    return false;
  size_t different_bytes_branch = e->code.len;
  if (!mach_u8(e, 0) || !mach_u8(e, 0x45) || !mach_u8(e, 0x85) ||
      !mach_u8(e, 0xd2) || !mach_u8(e, 0x74))
    return false;
  size_t nul_branch = e->code.len;
  if (!mach_u8(e, 0) || !mach_u8(e, 0x49) || !mach_u8(e, 0xff) ||
      !mach_u8(e, 0xc4) || !mach_u8(e, 0x49) || !mach_u8(e, 0xff) ||
      !mach_u8(e, 0xc5) || !mach_u8(e, 0xeb))
    return false;
  size_t loop_branch = e->code.len;
  if (!mach_u8(e, 0))
    return false;
  size_t different = e->code.len;
  if (!mach_u8(e, 0x44) || !mach_u8(e, 0x89) || !mach_u8(e, 0xd0) ||
      !mach_u8(e, 0x44) || !mach_u8(e, 0x29) || !mach_u8(e, 0xd8) ||
      !mach_u8(e, 0x48) || !mach_u8(e, 0x63) || !mach_u8(e, 0xc0) ||
      !mach_u8(e, 0xeb))
    return false;
  size_t different_finish_branch = e->code.len;
  if (!mach_u8(e, 0))
    return false;
  size_t same = e->code.len;
  if (!mach_u8(e, 0x31) || !mach_u8(e, 0xc0))
    return false;
  size_t finish = e->code.len;
  if (!mach_patch_rel8(e, same_pointer_branch, same) ||
      !mach_patch_rel8(e, left_nonnull_branch, right_null_check) ||
      !mach_patch_rel8(e, null_left_branch, e->code.len) ||
      !mach_patch_rel8(e, right_nonnull_branch, loop) ||
      !mach_patch_rel8(e, null_right_branch, e->code.len) ||
      !mach_patch_rel8(e, different_bytes_branch, different) ||
      !mach_patch_rel8(e, nul_branch, same) ||
      !mach_patch_rel8(e, loop_branch, loop) ||
      !mach_patch_rel8(e, different_finish_branch, finish) ||
      !mach_u8(e, 0x48) || !mach_u8(e, 0x83) || !mach_u8(e, 0xc4) ||
      !mach_u8(e, 0x10) || !mach_u8(e, 0x41) || !mach_u8(e, 0x5d) ||
      !mach_u8(e, 0x41) || !mach_u8(e, 0x5c))
    return false;
  return mach_bare_call_finish(e, in);
}

static bool mach_bare_cstr_len(nyr_x64_mach_enc_t *e,
                               const nyr_mach_inst_t *in) {
  if (!e || !in || in->args_len != 1)
    return mach_err(e, "x64 bare C-string length expects one argument");
  if (!mach_load_call_arg_rax(e, &in->args[0]) || !mach_mov_reg_rax(e, 7) ||
      !mach_u8(e, 0x31) || !mach_u8(e, 0xc0) || !mach_u8(e, 0x48) ||
      !mach_u8(e, 0x85) || !mach_u8(e, 0xff) || !mach_u8(e, 0x74))
    return false;
  size_t null_branch = e->code.len;
  if (!mach_u8(e, 0))
    return false;
  size_t loop = e->code.len;
  if (!mach_u8(e, 0x80) || !mach_u8(e, 0x3c) || !mach_u8(e, 0x07) ||
      !mach_u8(e, 0) || !mach_u8(e, 0x74))
    return false;
  size_t end_branch = e->code.len;
  if (!mach_u8(e, 0) || !mach_u8(e, 0x48) || !mach_u8(e, 0xff) ||
      !mach_u8(e, 0xc0) || !mach_u8(e, 0xeb))
    return false;
  size_t loop_branch = e->code.len;
  if (!mach_u8(e, 0) || !mach_patch_rel8(e, null_branch, e->code.len) ||
      !mach_patch_rel8(e, end_branch, e->code.len) ||
      !mach_patch_rel8(e, loop_branch, loop))
    return false;
  return mach_bare_call_finish(e, in);
}

static bool mach_bare_cstr_concat(nyr_x64_mach_enc_t *e,
                                  const nyr_mach_inst_t *in) {
  if (!e || !in || in->args_len != 2)
    return mach_err(e, "x64 bare C-string concatenation expects two arguments");
  /*
   * Keep the common 32-byte heap prefix; its tail stores tagged byte length
   * and TAG_STR before the NUL-terminated payload.
   */
  if (!mach_u8(e, 0x53) || !mach_u8(e, 0x41) || !mach_u8(e, 0x54) ||
      !mach_u8(e, 0x41) || !mach_u8(e, 0x55) || !mach_u8(e, 0x41) ||
      !mach_u8(e, 0x56) || !mach_u8(e, 0x41) || !mach_u8(e, 0x57) ||
      /*
       * Read both operands before any scratch register changes: either may
       * be colored in rbx/r12-r15, which the length loops below overwrite
       * (a right operand living in r14 was read after r14 held the left
       * length, and the bare `a + " world"` faulted). push rax / push rax /
       * pop r13 / pop r12 leaves left in r12 and right in r13.
       */
      !mach_load_call_arg_rax(e, &in->args[0]) || !mach_u8(e, 0x50) ||
      !mach_load_call_arg_rax(e, &in->args[1]) || !mach_u8(e, 0x50) ||
      !mach_u8(e, 0x41) || !mach_u8(e, 0x5d) || !mach_u8(e, 0x41) ||
      !mach_u8(e, 0x5c) || !mach_u8(e, 0x4d) || !mach_u8(e, 0x31) ||
      !mach_u8(e, 0xf6) || !mach_u8(e, 0x4d) || !mach_u8(e, 0x85) ||
      !mach_u8(e, 0xe4) || !mach_u8(e, 0x74))
    return false;
  size_t left_empty = e->code.len;
  if (!mach_u8(e, 0))
    return false;
  size_t left_loop = e->code.len;
  if (!mach_u8(e, 0x43) || !mach_u8(e, 0x80) || !mach_u8(e, 0x3c) ||
      !mach_u8(e, 0x34) || !mach_u8(e, 0) || !mach_u8(e, 0x74))
    return false;
  size_t left_end = e->code.len;
  if (!mach_u8(e, 0) || !mach_u8(e, 0x49) || !mach_u8(e, 0xff) ||
      !mach_u8(e, 0xc6) || !mach_u8(e, 0xeb))
    return false;
  size_t left_next = e->code.len;
  if (!mach_u8(e, 0) || !mach_patch_rel8(e, left_next, left_loop) ||
      !mach_patch_rel8(e, left_empty, e->code.len) ||
      !mach_patch_rel8(e, left_end, e->code.len))
    return false;
  if (!mach_u8(e, 0x4d) || !mach_u8(e, 0x31) || !mach_u8(e, 0xff) ||
      !mach_u8(e, 0x4d) || !mach_u8(e, 0x85) || !mach_u8(e, 0xed) ||
      !mach_u8(e, 0x74))
    return false;
  size_t right_empty = e->code.len;
  if (!mach_u8(e, 0))
    return false;
  size_t right_loop = e->code.len;
  if (!mach_u8(e, 0x43) || !mach_u8(e, 0x80) || !mach_u8(e, 0x7c) ||
      !mach_u8(e, 0x3d) || !mach_u8(e, 0) || !mach_u8(e, 0) ||
      !mach_u8(e, 0x74))
    return false;
  size_t right_end = e->code.len;
  if (!mach_u8(e, 0) || !mach_u8(e, 0x49) || !mach_u8(e, 0xff) ||
      !mach_u8(e, 0xc7) || !mach_u8(e, 0xeb))
    return false;
  size_t right_next = e->code.len;
  if (!mach_u8(e, 0) || !mach_patch_rel8(e, right_next, right_loop) ||
      !mach_patch_rel8(e, right_empty, e->code.len) ||
      !mach_patch_rel8(e, right_end, e->code.len) || !mach_u8(e, 0x4c) ||
      !mach_u8(e, 0x89) || !mach_u8(e, 0xf0) || !mach_u8(e, 0x4c) ||
      !mach_u8(e, 0x01) || !mach_u8(e, 0xf8) || !mach_u8(e, 0x0f) ||
      !mach_u8(e, 0x82))
    return false;
  size_t sum_overflow = e->code.len;
  if (!mach_i32(e, 0) || !mach_u8(e, 0x48) || !mach_u8(e, 0x83) ||
      !mach_u8(e, 0xc0) || !mach_u8(e, 1) || !mach_u8(e, 0x0f) ||
      !mach_u8(e, 0x82))
    return false;
  size_t terminator_overflow = e->code.len;
  if (!mach_i32(e, 0) || !mach_u8(e, 0x48) || !mach_u8(e, 0x05) ||
      !mach_i32(e, 4095) || !mach_u8(e, 0x0f) || !mach_u8(e, 0x82))
    return false;
  size_t page_overflow = e->code.len;
  if (!mach_i32(e, 0) || !mach_u8(e, 0x48) || !mach_u8(e, 0x25) ||
      !mach_i32(e, INT32_C(0xfffff000)) || !mach_u8(e, 0x48) ||
      !mach_u8(e, 0x89) || !mach_u8(e, 0xc6) || !mach_u8(e, 0x31) ||
      !mach_u8(e, 0xff) || !mach_mov_preg_imm(e, 2, 3) ||
      !mach_mov_preg_imm(e, 10, 34) || !mach_mov_preg_imm(e, 8, -1) ||
      !mach_u8(e, 0x45) || !mach_u8(e, 0x31) || !mach_u8(e, 0xc9) ||
      !mach_mov_preg_imm(e, 0, e->target->syscalls.mmap_nr) ||
      !mach_u8(e, 0x0f) || !mach_u8(e, 0x05) || !mach_u8(e, 0x48) ||
      !mach_u8(e, 0x85) || !mach_u8(e, 0xc0) || !mach_u8(e, 0x0f) ||
      !mach_u8(e, 0x88))
    return false;
  size_t map_failed = e->code.len;
  if (!mach_i32(e, 0) || !mach_u8(e, 0x48) || !mach_u8(e, 0x89) ||
      !mach_u8(e, 0xc3) ||
      !mach_mov_preg_imm(e, 10, INT64_C(0x545249584e5954)) ||
      !mach_u8(e, 0x4c) || !mach_u8(e, 0x89) || !mach_u8(e, 0x13) ||
      !mach_u8(e, 0x48) || !mach_u8(e, 0x89) || !mach_u8(e, 0x73) ||
      !mach_u8(e, 0x08) || !mach_u8(e, 0x4c) || !mach_u8(e, 0x89) ||
      !mach_u8(e, 0xf0) || !mach_u8(e, 0x4c) || !mach_u8(e, 0x01) ||
      !mach_u8(e, 0xf8) || !mach_u8(e, 0x48) || !mach_u8(e, 0xd1) ||
      !mach_u8(e, 0xe0) || !mach_u8(e, 0x48) || !mach_u8(e, 0x83) ||
      !mach_u8(e, 0xc8) || !mach_u8(e, 1) || !mach_u8(e, 0x48) ||
      !mach_u8(e, 0x89) || !mach_u8(e, 0x43) || !mach_u8(e, 0x10) ||
      !mach_u8(e, 0xc7) || !mach_u8(e, 0x43) || !mach_u8(e, 0x18) ||
      !mach_i32(e, 120) || !mach_u8(e, 0x4c) || !mach_u8(e, 0x89) ||
      !mach_u8(e, 0xe6) || !mach_u8(e, 0x48) || !mach_u8(e, 0x8d) ||
      !mach_u8(e, 0x7b) || !mach_u8(e, 0x20) || !mach_u8(e, 0x4c) ||
      !mach_u8(e, 0x89) || !mach_u8(e, 0xf1) || !mach_u8(e, 0xf3) ||
      !mach_u8(e, 0xa4) || !mach_u8(e, 0x4c) || !mach_u8(e, 0x89) ||
      !mach_u8(e, 0xee) || !mach_u8(e, 0x4a) || !mach_u8(e, 0x8d) ||
      !mach_u8(e, 0x7c) || !mach_u8(e, 0x33) || !mach_u8(e, 0x20) ||
      !mach_u8(e, 0x4c) || !mach_u8(e, 0x89) || !mach_u8(e, 0xf9) ||
      !mach_u8(e, 0xf3) || !mach_u8(e, 0xa4) || !mach_u8(e, 0xc6) ||
      !mach_u8(e, 0x07) || !mach_u8(e, 0) || !mach_u8(e, 0x48) ||
      !mach_u8(e, 0x8d) || !mach_u8(e, 0x43) || !mach_u8(e, 0x20) ||
      !mach_u8(e, 0xeb))
    return false;
  size_t success_branch = e->code.len;
  if (!mach_u8(e, 0))
    return false;
  size_t failure = e->code.len;
  if (!mach_u8(e, 0x31) || !mach_u8(e, 0xc0))
    return false;
  size_t finish = e->code.len;
  if (!mach_u8(e, 0x41) || !mach_u8(e, 0x5f) || !mach_u8(e, 0x41) ||
      !mach_u8(e, 0x5e) || !mach_u8(e, 0x41) || !mach_u8(e, 0x5d) ||
      !mach_u8(e, 0x41) || !mach_u8(e, 0x5c) || !mach_u8(e, 0x5b) ||
      !mach_patch_rel32(e, sum_overflow, failure) ||
      !mach_patch_rel32(e, terminator_overflow, failure) ||
      !mach_patch_rel32(e, page_overflow, failure) ||
      !mach_patch_rel32(e, map_failed, failure) ||
      !mach_patch_rel8(e, success_branch, finish))
    return false;
  return mach_bare_call_finish(e, in);
}

/*
 * Freestanding legalization of the shared string-repeat operation.
 */
static bool mach_bare_str_repeat(nyr_x64_mach_enc_t *e,
                                 const nyr_mach_inst_t *in) {
  if (!e || !in || in->args_len != 2)
    return mach_err(e, "x64 bare string repeat expects two arguments");
  /*
   * Keep values in callee-saved registers across mmap's syscall clobbers.
   */
  if (!mach_u8(e, 0x53) || !mach_u8(e, 0x41) || !mach_u8(e, 0x54) ||
      !mach_u8(e, 0x41) || !mach_u8(e, 0x55) || !mach_u8(e, 0x41) ||
      !mach_u8(e, 0x56) || !mach_u8(e, 0x41) || !mach_u8(e, 0x57) ||
      !mach_load_call_arg_rax(e, &in->args[0]) || !mach_mov_reg_rax(e, 12) ||
      !mach_load_call_arg_rax(e, &in->args[1]) || !mach_mov_reg_rax(e, 13) ||
      !mach_u8(e, 0x4d) || !mach_u8(e, 0x85) || !mach_u8(e, 0xe4) ||
      !mach_u8(e, 0x0f) || !mach_u8(e, 0x84))
    return false;
  size_t null_source = e->code.len;
  if (!mach_i32(e, 0) || !mach_u8(e, 0x49) || !mach_u8(e, 0x83) ||
      !mach_u8(e, 0xfd) || !mach_u8(e, 0) || !mach_u8(e, 0x0f) ||
      !mach_u8(e, 0x8e))
    return false;
  size_t nonpositive_count = e->code.len;
  if (!mach_i32(e, 0) || !mach_u8(e, 0x4d) || !mach_u8(e, 0x31) ||
      !mach_u8(e, 0xf6))
    return false;
  size_t length_loop = e->code.len;
  if (!mach_u8(e, 0x43) || !mach_u8(e, 0x80) || !mach_u8(e, 0x3c) ||
      !mach_u8(e, 0x34) || !mach_u8(e, 0) || !mach_u8(e, 0x0f) ||
      !mach_u8(e, 0x84))
    return false;
  size_t length_done_branch = e->code.len;
  if (!mach_i32(e, 0) || !mach_u8(e, 0x49) || !mach_u8(e, 0xff) ||
      !mach_u8(e, 0xc6) || !mach_u8(e, 0xe9))
    return false;
  size_t length_loop_branch = e->code.len;
  if (!mach_i32(e, 0) ||
      !mach_patch_rel32(e, length_loop_branch, length_loop) ||
      !mach_patch_rel32(e, length_done_branch, e->code.len) ||
      !mach_u8(e, 0x4d) || !mach_u8(e, 0x85) || !mach_u8(e, 0xf6) ||
      !mach_u8(e, 0x0f) || !mach_u8(e, 0x84))
    return false;
  size_t empty_source = e->code.len;
  if (!mach_i32(e, 0) || !mach_u8(e, 0x4c) || !mach_u8(e, 0x89) ||
      !mach_u8(e, 0xf0) || !mach_u8(e, 0x49) || !mach_u8(e, 0x0f) ||
      !mach_u8(e, 0xaf) || !mach_u8(e, 0xc5) || !mach_u8(e, 0x0f) ||
      !mach_u8(e, 0x80))
    return false;
  size_t multiply_overflow = e->code.len;
  if (!mach_i32(e, 0) || !mach_u8(e, 0x49) || !mach_u8(e, 0x89) ||
      !mach_u8(e, 0xc7) || !mach_u8(e, 0x48) || !mach_u8(e, 0x05) ||
      !mach_i32(e, 4096) || !mach_u8(e, 0x0f) || !mach_u8(e, 0x80))
    return false;
  size_t allocation_overflow = e->code.len;
  if (!mach_i32(e, 0) || !mach_u8(e, 0x48) || !mach_u8(e, 0x25) ||
      !mach_i32(e, INT32_C(0xfffff000)) || !mach_u8(e, 0x48) ||
      !mach_u8(e, 0x89) || !mach_u8(e, 0xc6) || !mach_u8(e, 0x31) ||
      !mach_u8(e, 0xff) || !mach_mov_preg_imm(e, 2, 3) ||
      !mach_mov_preg_imm(e, 10, 34) || !mach_mov_preg_imm(e, 8, -1) ||
      !mach_u8(e, 0x45) || !mach_u8(e, 0x31) || !mach_u8(e, 0xc9) ||
      !mach_mov_preg_imm(e, 0, e->target->syscalls.mmap_nr) ||
      !mach_u8(e, 0x0f) || !mach_u8(e, 0x05) || !mach_u8(e, 0x48) ||
      !mach_u8(e, 0x3d) || !mach_i32(e, -4095) || !mach_u8(e, 0x0f) ||
      !mach_u8(e, 0x83))
    return false;
  size_t mmap_failed = e->code.len;
  if (!mach_i32(e, 0) || !mach_u8(e, 0x48) || !mach_u8(e, 0x89) ||
      !mach_u8(e, 0xc3) || !mach_u8(e, 0x48) || !mach_u8(e, 0x89) ||
      !mach_u8(e, 0xdf))
    return false;
  size_t copy_loop = e->code.len;
  if (!mach_u8(e, 0x4c) || !mach_u8(e, 0x89) || !mach_u8(e, 0xe6) ||
      !mach_u8(e, 0x4c) || !mach_u8(e, 0x89) || !mach_u8(e, 0xf1) ||
      !mach_u8(e, 0xf3) || !mach_u8(e, 0xa4) || !mach_u8(e, 0x49) ||
      !mach_u8(e, 0xff) || !mach_u8(e, 0xcd) || !mach_u8(e, 0x0f) ||
      !mach_u8(e, 0x85))
    return false;
  size_t copy_loop_branch = e->code.len;
  if (!mach_i32(e, 0) || !mach_u8(e, 0xc6) || !mach_u8(e, 0x07) ||
      !mach_u8(e, 0) || !mach_u8(e, 0x48) || !mach_u8(e, 0x89) ||
      !mach_u8(e, 0xd8) || !mach_u8(e, 0xe9))
    return false;
  size_t success_branch = e->code.len;
  if (!mach_i32(e, 0))
    return false;
  size_t empty = e->code.len;
  if (!mach_u8(e, 0x31) || !mach_u8(e, 0xff) ||
      !mach_mov_preg_imm(e, 6, 4096) || !mach_mov_preg_imm(e, 2, 3) ||
      !mach_mov_preg_imm(e, 10, 34) || !mach_mov_preg_imm(e, 8, -1) ||
      !mach_u8(e, 0x45) || !mach_u8(e, 0x31) || !mach_u8(e, 0xc9) ||
      !mach_mov_preg_imm(e, 0, e->target->syscalls.mmap_nr) ||
      !mach_u8(e, 0x0f) || !mach_u8(e, 0x05) || !mach_u8(e, 0x48) ||
      !mach_u8(e, 0x3d) || !mach_i32(e, -4095) || !mach_u8(e, 0x0f) ||
      !mach_u8(e, 0x83))
    return false;
  size_t empty_alloc_failed = e->code.len;
  if (!mach_i32(e, 0) || !mach_u8(e, 0xc6) || !mach_u8(e, 0x00) ||
      !mach_u8(e, 0) || !mach_u8(e, 0xe9))
    return false;
  size_t empty_success_branch = e->code.len;
  if (!mach_i32(e, 0))
    return false;
  size_t empty_failed = e->code.len;
  if (!mach_u8(e, 0x31) || !mach_u8(e, 0xc0))
    return false;
  size_t finish = e->code.len;
  if (!mach_u8(e, 0x41) || !mach_u8(e, 0x5f) || !mach_u8(e, 0x41) ||
      !mach_u8(e, 0x5e) || !mach_u8(e, 0x41) || !mach_u8(e, 0x5d) ||
      !mach_u8(e, 0x41) || !mach_u8(e, 0x5c) || !mach_u8(e, 0x5b) ||
      !mach_patch_rel32(e, null_source, empty) ||
      !mach_patch_rel32(e, nonpositive_count, empty) ||
      !mach_patch_rel32(e, empty_source, empty) ||
      !mach_patch_rel32(e, multiply_overflow, empty) ||
      !mach_patch_rel32(e, allocation_overflow, empty) ||
      !mach_patch_rel32(e, mmap_failed, empty) ||
      !mach_patch_rel32(e, copy_loop_branch, copy_loop) ||
      !mach_patch_rel32(e, success_branch, finish) ||
      !mach_patch_rel32(e, empty_alloc_failed, empty_failed) ||
      !mach_patch_rel32(e, empty_success_branch, finish))
    return false;
  return mach_bare_call_finish(e, in);
}

/*
 * Freestanding UTF-8 scalar encoding for the shared string character op.
 */
static bool mach_bare_str_chr(nyr_x64_mach_enc_t *e,
                              const nyr_mach_inst_t *in) {
  static const uint8_t encode_utf8[] = {
      0x49, 0x81, 0xfc, 0x80, 0x00, 0x00, 0x00, 0x0f, 0x82, 0x93, 0x00, 0x00,
      0x00, 0x49, 0x81, 0xfc, 0x00, 0x08, 0x00, 0x00, 0x72, 0x6e, 0x49, 0x81,
      0xfc, 0x00, 0x00, 0x01, 0x00, 0x72, 0x3a, 0x4c, 0x89, 0xe0, 0x48, 0xc1,
      0xe8, 0x12, 0x0c, 0xf0, 0x88, 0x03, 0x4c, 0x89, 0xe0, 0x48, 0xc1, 0xe8,
      0x0c, 0x83, 0xe0, 0x3f, 0x0c, 0x80, 0x88, 0x43, 0x01, 0x4c, 0x89, 0xe0,
      0x48, 0xc1, 0xe8, 0x06, 0x83, 0xe0, 0x3f, 0x0c, 0x80, 0x88, 0x43, 0x02,
      0x4c, 0x89, 0xe0, 0x83, 0xe0, 0x3f, 0x0c, 0x80, 0x88, 0x43, 0x03, 0xc6,
      0x43, 0x04, 0x00, 0xeb, 0x4e, 0x4c, 0x89, 0xe0, 0x48, 0xc1, 0xe8, 0x0c,
      0x0c, 0xe0, 0x88, 0x03, 0x4c, 0x89, 0xe0, 0x48, 0xc1, 0xe8, 0x06, 0x83,
      0xe0, 0x3f, 0x0c, 0x80, 0x88, 0x43, 0x01, 0x4c, 0x89, 0xe0, 0x83, 0xe0,
      0x3f, 0x0c, 0x80, 0x88, 0x43, 0x02, 0xc6, 0x43, 0x03, 0x00, 0xeb, 0x23,
      0x4c, 0x89, 0xe0, 0x48, 0xc1, 0xe8, 0x06, 0x0c, 0xc0, 0x88, 0x03, 0x4c,
      0x89, 0xe0, 0x83, 0xe0, 0x3f, 0x0c, 0x80, 0x88, 0x43, 0x01, 0xc6, 0x43,
      0x02, 0x00, 0xeb, 0x07, 0x44, 0x88, 0x23, 0xc6, 0x43, 0x01, 0x00};
  if (!e || !in || in->args_len != 1)
    return mach_err(e, "x64 bare string character expects one argument");
  if (!mach_u8(e, 0x53) || !mach_u8(e, 0x41) || !mach_u8(e, 0x54) ||
      !mach_load_call_arg_rax(e, &in->args[0]) || !mach_mov_reg_rax(e, 12) ||
      !mach_u8(e, 0x49) || !mach_u8(e, 0x83) || !mach_u8(e, 0xfc) ||
      !mach_u8(e, 0) || !mach_u8(e, 0x0f) || !mach_u8(e, 0x8c))
    return false;
  size_t negative = e->code.len;
  if (!mach_i32(e, 0) || !mach_u8(e, 0x49) || !mach_u8(e, 0x81) ||
      !mach_u8(e, 0xfc) || !mach_i32(e, 0x10ffff) || !mach_u8(e, 0x0f) ||
      !mach_u8(e, 0x87))
    return false;
  size_t above_unicode = e->code.len;
  if (!mach_i32(e, 0) || !mach_u8(e, 0x49) || !mach_u8(e, 0x81) ||
      !mach_u8(e, 0xfc) || !mach_i32(e, 0xd800) || !mach_u8(e, 0x0f) ||
      !mach_u8(e, 0x82))
    return false;
  size_t below_surrogate = e->code.len;
  if (!mach_i32(e, 0) || !mach_u8(e, 0x49) || !mach_u8(e, 0x81) ||
      !mach_u8(e, 0xfc) || !mach_i32(e, 0xdfff) || !mach_u8(e, 0x0f) ||
      !mach_u8(e, 0x86))
    return false;
  size_t in_surrogate = e->code.len;
  if (!mach_i32(e, 0))
    return false;
  size_t allocation = e->code.len;
  if (!mach_u8(e, 0x31) || !mach_u8(e, 0xff) ||
      !mach_mov_preg_imm(e, 6, 4096) || !mach_mov_preg_imm(e, 2, 3) ||
      !mach_mov_preg_imm(e, 10, 34) || !mach_mov_preg_imm(e, 8, -1) ||
      !mach_u8(e, 0x45) || !mach_u8(e, 0x31) || !mach_u8(e, 0xc9) ||
      !mach_mov_preg_imm(e, 0, e->target->syscalls.mmap_nr) ||
      !mach_u8(e, 0x0f) || !mach_u8(e, 0x05) || !mach_u8(e, 0x48) ||
      !mach_u8(e, 0x3d) || !mach_i32(e, -4095) || !mach_u8(e, 0x0f) ||
      !mach_u8(e, 0x83))
    return false;
  size_t allocation_failed = e->code.len;
  if (!mach_i32(e, 0) || !mach_u8(e, 0x48) || !mach_u8(e, 0x89) ||
      !mach_u8(e, 0xc3) || !mach_bytes(e, encode_utf8, sizeof(encode_utf8)) ||
      !mach_u8(e, 0x48) || !mach_u8(e, 0x89) || !mach_u8(e, 0xd8) ||
      !mach_u8(e, 0xe9))
    return false;
  size_t success = e->code.len;
  if (!mach_i32(e, 0))
    return false;
  size_t invalid = e->code.len;
  if (!mach_u8(e, 0x31) || !mach_u8(e, 0xc0))
    return false;
  size_t finish = e->code.len;
  if (!mach_u8(e, 0x41) || !mach_u8(e, 0x5c) || !mach_u8(e, 0x5b) ||
      !mach_patch_rel32(e, negative, invalid) ||
      !mach_patch_rel32(e, above_unicode, invalid) ||
      !mach_patch_rel32(e, below_surrogate, allocation) ||
      !mach_patch_rel32(e, in_surrogate, invalid) ||
      !mach_patch_rel32(e, allocation_failed, invalid) ||
      !mach_patch_rel32(e, success, finish))
    return false;
  return mach_bare_call_finish(e, in);
}

#include "code/backend/mach/arch/x86/payload.h"

/*
 * One self-contained PIC payload per function, skipped on normal fallthrough.
 * Its SysV callee preserves the allocator's callee-saved homes and owns its
 * scratch frame. Calls use the same aligned stack as ordinary native calls.
 */
static bool mach_bare_payload(nyr_x64_mach_enc_t *e,
                              const nyr_mach_inst_t *in) {
  unsigned argc = 0;
  unsigned op = ny_bare_payload_op(in->runtime_operation, &argc);
  if (!op || in->args_len != argc)
    return mach_err(e, "x64 bare runtime payload argument mismatch");
  if (!e->bare_payload_entry) {
    if (!mach_u8(e, 0xe9))
      return false;
    size_t skip = e->code.len;
    if (!mach_i32(e, 0))
      return false;
    size_t base = e->code.len;
    if (!mach_bytes(e, ny_bare_x86_64_code, sizeof(ny_bare_x86_64_code)) ||
        !mach_patch_rel32(e, skip, e->code.len))
      return false;
    e->bare_payload_entry = base + ny_bare_x86_64_entry + 1;
  }
  /*
   * Stage through the stack so operand registers cannot overwrite earlier
   * ABI arguments. All homes use rbp, and four pushes preserve alignment.
   */
  for (unsigned i = 0; i < 4; ++i) {
    if (i < argc) {
      if (!mach_load_call_arg_rax(e, &in->args[i]))
        return false;
    } else if (!mach_mov_preg_imm(e, 0, 0))
      return false;
    if (!mach_u8(e, 0x50))
      return false;
  }
  if (!mach_u8(e, 0x59) || /* pop rcx (arg 4) */
      !mach_u8(e, 0x5a) || !mach_u8(e, 0x5e) || !mach_u8(e, 0x5f) ||
      !mach_mov_preg_imm(e, 8, op) || !mach_u8(e, 0xe8))
    return false;
  size_t call = e->code.len;
  if (!mach_i32(e, 0) || !mach_patch_rel32(e, call, e->bare_payload_entry - 1))
    return false;
  return mach_bare_call_finish(e, in);
}

static bool mach_bare_raw_memory(nyr_x64_mach_enc_t *e,
                                 const nyr_mach_inst_t *in,
                                 ny_native_runtime_op_t operation) {
  if (!e || !in || in->args_len != 3 ||
      (operation != NY_NATIVE_RUNTIME_OP_MEMCPY &&
       operation != NY_NATIVE_RUNTIME_OP_MEMCMP &&
       operation != NY_NATIVE_RUNTIME_OP_MEMSET))
    return mach_err(e, "x64 bare raw memory helper expects three arguments");

  if (operation == NY_NATIVE_RUNTIME_OP_MEMSET) {
    if (!mach_load_call_arg_rax(e, &in->args[0]) || !mach_mov_reg_rax(e, 8) ||
        !mach_mov_reg_rax(e, 7) || !mach_load_call_arg_rax(e, &in->args[1]) ||
        !mach_mov_reg_rax(e, 6) || !mach_load_call_arg_rax(e, &in->args[2]) ||
        !mach_mov_reg_rax(e, 2) || !mach_u8(e, 0x48) || !mach_u8(e, 0x85) ||
        !mach_u8(e, 0xd2) || !mach_u8(e, 0x0f) || !mach_u8(e, 0x8e))
      return false;
    size_t done_branch = e->code.len;
    if (!mach_i32(e, 0) || !mach_u8(e, 0x89) || !mach_u8(e, 0xf0) ||
        !mach_u8(e, 0x48) || !mach_u8(e, 0x89) || !mach_u8(e, 0xd1) ||
        !mach_u8(e, 0xf3) || !mach_u8(e, 0xaa) || !mach_u8(e, 0x4c) ||
        !mach_u8(e, 0x89) || !mach_u8(e, 0xc0))
      return false;
    size_t done = e->code.len;
    return mach_patch_rel32(e, done_branch, done) &&
           mach_bare_call_finish(e, in);
  }

  if (!mach_load_call_arg_rax(e, &in->args[0]) || !mach_mov_reg_rax(e, 7) ||
      !mach_load_call_arg_rax(e, &in->args[1]) || !mach_mov_reg_rax(e, 6) ||
      !mach_load_call_arg_rax(e, &in->args[2]) || !mach_mov_reg_rax(e, 2) ||
      !mach_u8(e, 0x4c) || !mach_u8(e, 0x8b) || !mach_u8(e, 0xc7) ||
      !mach_u8(e, 0x48) || !mach_u8(e, 0x85) || !mach_u8(e, 0xd2) ||
      !mach_u8(e, 0x0f) || !mach_u8(e, 0x8e))
    return false;
  size_t empty_branch = e->code.len;
  if (!mach_i32(e, 0))
    return false;
  size_t loop = e->code.len;
  if (operation == NY_NATIVE_RUNTIME_OP_MEMCPY) {
    if (!mach_u8(e, 0x8a) || !mach_u8(e, 0x06) || !mach_u8(e, 0x88) ||
        !mach_u8(e, 0x07))
      return false;
  } else if (!mach_u8(e, 0x0f) || !mach_u8(e, 0xb6) || !mach_u8(e, 0x07) ||
             !mach_u8(e, 0x0f) || !mach_u8(e, 0xb6) || !mach_u8(e, 0x0e) ||
             !mach_u8(e, 0x39) || !mach_u8(e, 0xc8) || !mach_u8(e, 0x0f) ||
             !mach_u8(e, 0x85))
    return false;
  size_t mismatch_branch =
      operation == NY_NATIVE_RUNTIME_OP_MEMCMP ? e->code.len : SIZE_MAX;
  if (mismatch_branch != SIZE_MAX && !mach_i32(e, 0))
    return false;
  if (!mach_u8(e, 0x48) || !mach_u8(e, 0xff) || !mach_u8(e, 0xc6) ||
      !mach_u8(e, 0x48) || !mach_u8(e, 0xff) || !mach_u8(e, 0xc7) ||
      !mach_u8(e, 0x48) || !mach_u8(e, 0xff) || !mach_u8(e, 0xca) ||
      !mach_u8(e, 0x75))
    return false;
  size_t loop_branch = e->code.len;
  if (!mach_u8(e, 0))
    return false;
  size_t equal_result = SIZE_MAX;
  if (operation == NY_NATIVE_RUNTIME_OP_MEMCMP) {
    equal_result = e->code.len;
    if (!mach_u8(e, 0x31) || !mach_u8(e, 0xc0) || !mach_u8(e, 0xe9))
      return false;
  }
  size_t equal_branch =
      operation == NY_NATIVE_RUNTIME_OP_MEMCMP ? e->code.len : SIZE_MAX;
  if (equal_branch != SIZE_MAX && !mach_i32(e, 0))
    return false;
  size_t mismatch = e->code.len;
  if (operation == NY_NATIVE_RUNTIME_OP_MEMCMP &&
      (!mach_u8(e, 0x29) || !mach_u8(e, 0xc8) || !mach_u8(e, 0x48) ||
       !mach_u8(e, 0x63) || !mach_u8(e, 0xc0)))
    return false;
  if (operation != NY_NATIVE_RUNTIME_OP_MEMCMP &&
      (!mach_u8(e, 0x4c) || !mach_u8(e, 0x89) || !mach_u8(e, 0xc0)))
    return false;
  size_t done = e->code.len;
  if (!mach_patch_rel32(e, empty_branch,
                        operation == NY_NATIVE_RUNTIME_OP_MEMCMP ? equal_result
                                                                 : done) ||
      !mach_patch_rel8(e, loop_branch, loop))
    return false;
  if (mismatch_branch != SIZE_MAX &&
      (!mach_patch_rel32(e, mismatch_branch, mismatch) ||
       !mach_patch_rel32(e, equal_branch, done)))
    return false;
  return mach_bare_call_finish(e, in);
}

static bool mach_bare_heap(nyr_x64_mach_enc_t *e, const nyr_mach_inst_t *in) {
  if (!e || !in || in->args_len != 1 ||
      in->src0.kind != NYR_MACH_OPERAND_SYMBOL)
    return mach_err(e, "x64 bare heap helper expects one argument");
  const char *symbol = in->src0.as.symbol;
  bool allocate = strcmp(symbol, "rt_malloc_i64") == 0;
  bool deallocate =
      strcmp(symbol, "rt_free") == 0 || strcmp(symbol, "rt_free_raw") == 0;
  bool untag = strcmp(symbol, "rt_untag") == 0;
  if (!allocate && !deallocate && !untag)
    return false;
  if (untag) {
    if (!mach_load_call_arg_rax(e, &in->args[0]) || !mach_u8(e, 0xa8) ||
        !mach_u8(e, 1) || !mach_u8(e, 0x74))
      return false;
    size_t already_raw = e->code.len;
    if (!mach_u8(e, 0x04) || !mach_u8(e, 0x48) || !mach_u8(e, 0xd1) ||
        !mach_u8(e, 0xf8))
      return false;
    size_t done = e->code.len;
    if (!mach_patch_rel8(e, already_raw, done))
      return false;
    return mach_bare_call_finish(e, in);
  }
  size_t fail_branch = SIZE_MAX;
  if (!mach_u8(e, 0x41) || !mach_u8(e, 0x54) ||
      !mach_load_call_arg_rax(e, &in->args[0]))
    return false;
  if (allocate) {
    /*
     * Reserve room for the payload and the bare heap header.
     */
    if (!mach_u8(e, 0x48) || !mach_u8(e, 0x85) || !mach_u8(e, 0xc0) ||
        !mach_u8(e, 0x0f) || !mach_u8(e, 0x88))
      return false;
    fail_branch = e->code.len;
    if (!mach_i32(e, 0) || !mach_u8(e, 0x48) || !mach_u8(e, 0x05) ||
        !mach_i32(e, 4127) || !mach_u8(e, 0x0f) || !mach_u8(e, 0x82))
      return false;
    size_t overflow_branch = e->code.len;
    if (!mach_i32(e, 0) ||
        !mach_mov_preg_imm(e, 10, INT64_C(0xfffffffffffff000)) ||
        !mach_u8(e, 0x4c) || !mach_u8(e, 0x21) || !mach_u8(e, 0xd0) ||
        !mach_mov_reg_rax(e, 12) || !mach_u8(e, 0x31) || !mach_u8(e, 0xff) ||
        !mach_u8(e, 0x4c) || !mach_u8(e, 0x89) || !mach_u8(e, 0xe6) ||
        !mach_mov_preg_imm(e, 2, 3) || !mach_mov_preg_imm(e, 10, 34) ||
        !mach_mov_preg_imm(e, 8, -1) || !mach_u8(e, 0x45) ||
        !mach_u8(e, 0x31) || !mach_u8(e, 0xc9) ||
        !mach_mov_preg_imm(e, 0, e->target->syscalls.mmap_nr) ||
        !mach_u8(e, 0x0f) || !mach_u8(e, 0x05) || !mach_u8(e, 0x48) ||
        !mach_u8(e, 0x85) || !mach_u8(e, 0xc0) || !mach_u8(e, 0x0f) ||
        !mach_u8(e, 0x88))
      return false;
    size_t mmap_failure = e->code.len;
    if (!mach_i32(e, 0) ||
        !mach_mov_preg_imm(e, 10, INT64_C(0x545249584e5954)) ||
        !mach_u8(e, 0x4c) || !mach_u8(e, 0x89) || !mach_u8(e, 0x10) ||
        !mach_u8(e, 0x4c) || !mach_u8(e, 0x89) || !mach_u8(e, 0x60) ||
        !mach_u8(e, 0x08) || !mach_u8(e, 0x48) || !mach_u8(e, 0x83) ||
        !mach_u8(e, 0xc0) || !mach_u8(e, 0x20) || !mach_u8(e, 0xeb))
      return false;
    size_t success_branch = e->code.len;
    if (!mach_u8(e, 0))
      return false;
    size_t zero_case = e->code.len;
    if (!mach_u8(e, 0x31) || !mach_u8(e, 0xc0))
      return false;
    size_t finish = e->code.len;
    if (!mach_u8(e, 0x41) || !mach_u8(e, 0x5c) ||
        !mach_patch_rel32(e, fail_branch, zero_case) ||
        !mach_patch_rel32(e, overflow_branch, zero_case) ||
        !mach_patch_rel32(e, mmap_failure, zero_case) ||
        !mach_patch_rel8(e, success_branch, finish))
      return false;
  } else {
    if (!mach_u8(e, 0x48) || !mach_u8(e, 0x85) || !mach_u8(e, 0xc0) ||
        !mach_u8(e, 0x0f) || !mach_u8(e, 0x84))
      return false;
    size_t null_branch = e->code.len;
    if (!mach_i32(e, 0) || !mach_u8(e, 0x48) || !mach_u8(e, 0x83) ||
        !mach_u8(e, 0xe8) || !mach_u8(e, 0x20) ||
        !mach_mov_preg_imm(e, 10, INT64_C(0x545249584e5954)) ||
        !mach_u8(e, 0x4c) || !mach_u8(e, 0x39) || !mach_u8(e, 0x10) ||
        !mach_u8(e, 0x0f) || !mach_u8(e, 0x85))
      return false;
    size_t invalid_branch = e->code.len;
    if (!mach_i32(e, 0) || !mach_u8(e, 0x48) || !mach_u8(e, 0x8b) ||
        !mach_u8(e, 0x70) || !mach_u8(e, 0x08) || !mach_u8(e, 0x48) ||
        !mach_u8(e, 0x89) || !mach_u8(e, 0xc7) ||
        !mach_mov_preg_imm(e, 0, e->target->syscalls.munmap_nr) ||
        !mach_u8(e, 0x0f) || !mach_u8(e, 0x05) || !mach_u8(e, 0x48) ||
        !mach_u8(e, 0x83) || !mach_u8(e, 0xf8) || !mach_u8(e, 0) ||
        !mach_u8(e, 0x0f) || !mach_u8(e, 0x94) || !mach_u8(e, 0xc0) ||
        !mach_u8(e, 0x48) || !mach_u8(e, 0x0f) || !mach_u8(e, 0xb6) ||
        !mach_u8(e, 0xc0) || !mach_u8(e, 0xeb))
      return false;
    size_t finish_branch = e->code.len;
    if (!mach_u8(e, 0))
      return false;
    size_t null_case = e->code.len;
    if (!mach_u8(e, 0x31) || !mach_u8(e, 0xc0))
      return false;
    size_t finish = e->code.len;
    if (!mach_u8(e, 0x41) || !mach_u8(e, 0x5c) ||
        !mach_patch_rel32(e, null_branch, null_case) ||
        !mach_patch_rel32(e, invalid_branch, null_case) ||
        !mach_patch_rel8(e, finish_branch, finish))
      return false;
  }
  return mach_bare_call_finish(e, in);
}

static bool mach_bare_print_i64(nyr_x64_mach_enc_t *e,
                                const nyr_mach_inst_t *in, int fd) {
  if (!e || !in || in->args_len != 1)
    return mach_err(e, "x64 bare integer print expects one argument");
  size_t sign_branch, digit_loop, digit_branch, no_sign_branch;
  if (!mach_u8(e, 0x41) || !mach_u8(e, 0x54) || /* push r12 */
      !mach_u8(e, 0x48) || !mach_u8(e, 0x83) || !mach_u8(e, 0xec) ||
      !mach_u8(e, 0x20) || !mach_load_call_arg_rax(e, &in->args[0]) ||
      !mach_mov_reg_rax(e, 12) || !mach_u8(e, 0x48) || !mach_u8(e, 0x8d) ||
      !mach_u8(e, 0x74) || !mach_u8(e, 0x24) || !mach_u8(e, 0x1f) ||
      !mach_u8(e, 0xc6) || !mach_u8(e, 0x06) || !mach_u8(e, 0) ||
      !mach_u8(e, 0x4c) || !mach_u8(e, 0x89) || !mach_u8(e, 0xe0) ||
      !mach_u8(e, 0x45) || !mach_u8(e, 0x31) || !mach_u8(e, 0xc0) ||
      !mach_u8(e, 0x48) || !mach_u8(e, 0x85) || !mach_u8(e, 0xc0) ||
      !mach_u8(e, 0x79))
    return false;
  sign_branch = e->code.len;
  if (!mach_u8(e, 0) || !mach_u8(e, 0x41) || !mach_u8(e, 0xb8) ||
      !mach_i32(e, 1) || !mach_u8(e, 0x48) || !mach_u8(e, 0xf7) ||
      !mach_u8(e, 0xd8))
    return false;
  if (!mach_patch_rel8(e, sign_branch, e->code.len))
    return false;
  digit_loop = e->code.len;
  if (!mach_u8(e, 0x31) || !mach_u8(e, 0xd2) || !mach_u8(e, 0x41) ||
      !mach_u8(e, 0xb9) || !mach_i32(e, 10) || !mach_u8(e, 0x49) ||
      !mach_u8(e, 0xf7) || !mach_u8(e, 0xf1) || !mach_u8(e, 0x80) ||
      !mach_u8(e, 0xc2) || !mach_u8(e, 0x30) || !mach_u8(e, 0x48) ||
      !mach_u8(e, 0xff) || !mach_u8(e, 0xce) || !mach_u8(e, 0x88) ||
      !mach_u8(e, 0x16) || !mach_u8(e, 0x48) || !mach_u8(e, 0x85) ||
      !mach_u8(e, 0xc0) || !mach_u8(e, 0x75))
    return false;
  digit_branch = e->code.len;
  if (!mach_u8(e, 0) || !mach_patch_rel8(e, digit_branch, digit_loop) ||
      !mach_u8(e, 0x45) || !mach_u8(e, 0x85) || !mach_u8(e, 0xc0) ||
      !mach_u8(e, 0x74))
    return false;
  no_sign_branch = e->code.len;
  if (!mach_u8(e, 0) || !mach_u8(e, 0x48) || !mach_u8(e, 0xff) ||
      !mach_u8(e, 0xce) || !mach_u8(e, 0xc6) || !mach_u8(e, 0x06) ||
      !mach_u8(e, 0x2d) || !mach_patch_rel8(e, no_sign_branch, e->code.len) ||
      !mach_u8(e, 0x48) || !mach_u8(e, 0x8d) || !mach_u8(e, 0x54) ||
      !mach_u8(e, 0x24) || !mach_u8(e, 0x1f) || !mach_u8(e, 0x48) ||
      !mach_u8(e, 0x29) || !mach_u8(e, 0xf2) ||
      !mach_mov_preg_imm(e, 0, e->target->syscalls.write_nr) ||
      !mach_mov_preg_imm(e, 7, fd) || !mach_u8(e, 0x0f) || !mach_u8(e, 0x05) ||
      !mach_u8(e, 0x31) || !mach_u8(e, 0xc0) || !mach_u8(e, 0x48) ||
      !mach_u8(e, 0x83) || !mach_u8(e, 0xc4) || !mach_u8(e, 0x20) ||
      !mach_u8(e, 0x41) || !mach_u8(e, 0x5c))
    return false;
  return mach_bare_call_finish(e, in);
}

static bool mach_bare_print_newline(nyr_x64_mach_enc_t *e,
                                    const nyr_mach_inst_t *in, int fd) {
  if (!e || !in || in->args_len != 0)
    return mach_err(e, "x64 bare newline print expects no arguments");
  if (!mach_u8(e, 0x48) || !mach_u8(e, 0x83) || !mach_u8(e, 0xec) ||
      !mach_u8(e, 0x10) || !mach_u8(e, 0xc6) || !mach_u8(e, 0x04) ||
      !mach_u8(e, 0x24) || !mach_u8(e, '\n') ||
      !mach_mov_preg_imm(e, 0, e->target->syscalls.write_nr) ||
      !mach_mov_preg_imm(e, 7, fd) || !mach_u8(e, 0x48) || !mach_u8(e, 0x89) ||
      !mach_u8(e, 0xe6) || !mach_mov_preg_imm(e, 2, 1) || !mach_u8(e, 0x0f) ||
      !mach_u8(e, 0x05) || !mach_u8(e, 0x31) || !mach_u8(e, 0xc0) ||
      !mach_u8(e, 0x48) || !mach_u8(e, 0x83) || !mach_u8(e, 0xc4) ||
      !mach_u8(e, 0x10))
    return false;
  return mach_bare_call_finish(e, in);
}

static bool mach_bare_bool_to_cstr(nyr_x64_mach_enc_t *e,
                                   const nyr_mach_inst_t *in) {
  if (!e || !in || in->args_len != 1)
    return mach_err(e, "x64 bare boolean formatting expects one argument");
  if (!mach_load_call_arg_rax(e, &in->args[0]) || !mach_u8(e, 0x48) ||
      !mach_u8(e, 0x83) || !mach_u8(e, 0xf8) || !mach_u8(e, 1) ||
      !mach_u8(e, 0x0f) || !mach_u8(e, 0x84))
    return false;
  size_t raw_true_branch = e->code.len;
  if (!mach_i32(e, 0) || !mach_u8(e, 0x48) || !mach_u8(e, 0x83) ||
      !mach_u8(e, 0xf8) || !mach_u8(e, 8) || !mach_u8(e, 0x0f) ||
      !mach_u8(e, 0x84))
    return false;
  size_t tagged_true_branch = e->code.len;
  if (!mach_i32(e, 0) || !mach_u8(e, 0x48) || !mach_u8(e, 0x8d) ||
      !mach_u8(e, 0x05))
    return false;
  size_t false_literal_reloc = e->code.len;
  if (!mach_i32(e, 0) || !mach_u8(e, 0xe9))
    return false;
  size_t false_done_branch = e->code.len;
  if (!mach_i32(e, 0))
    return false;
  size_t true_label = e->code.len;
  if (!mach_u8(e, 0x48) || !mach_u8(e, 0x8d) || !mach_u8(e, 0x05))
    return false;
  size_t true_literal_reloc = e->code.len;
  if (!mach_i32(e, 0) || !mach_u8(e, 0xe9))
    return false;
  size_t true_done_branch = e->code.len;
  if (!mach_i32(e, 0))
    return false;
  size_t false_literal = e->code.len;
  static const unsigned char false_text[] = {'f', 'a', 'l', 's', 'e', 0};
  static const unsigned char true_text[] = {'t', 'r', 'u', 'e', 0};
  if (!mach_bytes(e, false_text, sizeof(false_text)))
    return false;
  size_t true_literal = e->code.len;
  if (!mach_bytes(e, true_text, sizeof(true_text)))
    return false;
  size_t finish = e->code.len;
  if (!mach_patch_rel32(e, raw_true_branch, true_label) ||
      !mach_patch_rel32(e, tagged_true_branch, true_label) ||
      !mach_patch_rel32(e, false_literal_reloc, false_literal) ||
      !mach_patch_rel32(e, true_literal_reloc, true_literal) ||
      !mach_patch_rel32(e, false_done_branch, finish) ||
      !mach_patch_rel32(e, true_done_branch, finish))
    return false;
  return mach_bare_call_finish(e, in);
}

static bool mach_bare_monotonic_ns(nyr_x64_mach_enc_t *e,
                                   const nyr_mach_inst_t *in) {
  if (!e || !in || in->args_len != 0)
    return mach_err(e, "x64 bare monotonic clock expects no arguments");
  if (!mach_u8(e, 0x48) || !mach_u8(e, 0x83) || !mach_u8(e, 0xec) ||
      !mach_u8(e, 0x10) || !mach_mov_preg_imm(e, 7, 1) || !mach_u8(e, 0x48) ||
      !mach_u8(e, 0x89) || !mach_u8(e, 0xe6) || !mach_u8(e, 0xb8) ||
      !mach_u32(e, e->target->syscalls.clock_gettime_nr) || !mach_u8(e, 0x0f) ||
      !mach_u8(e, 0x05) || !mach_u8(e, 0x48) || !mach_u8(e, 0x85) ||
      !mach_u8(e, 0xc0) || !mach_u8(e, 0x0f) || !mach_u8(e, 0x88))
    return false;
  mach_rax_invalidate(e);
  size_t error_branch = e->code.len;
  if (!mach_i32(e, 0) || !mach_u8(e, 0x48) || !mach_u8(e, 0x8b) ||
      !mach_u8(e, 0x04) || !mach_u8(e, 0x24) || !mach_u8(e, 0x48) ||
      !mach_u8(e, 0x69) || !mach_u8(e, 0xc0) ||
      !mach_u32(e, UINT32_C(1000000000)) || !mach_u8(e, 0x48) ||
      !mach_u8(e, 0x03) || !mach_u8(e, 0x44) || !mach_u8(e, 0x24) ||
      !mach_u8(e, 0x08) || !mach_u8(e, 0xe9))
    return false;
  size_t done_branch = e->code.len;
  if (!mach_i32(e, 0) || !mach_u8(e, 0x31) || !mach_u8(e, 0xc0))
    return false;
  size_t done = e->code.len;
  if (!mach_patch_rel32(e, error_branch, done_branch + 4) ||
      !mach_patch_rel32(e, done_branch, done) || !mach_u8(e, 0x48) ||
      !mach_u8(e, 0x83) || !mach_u8(e, 0xc4) || !mach_u8(e, 0x10))
    return false;
  mach_rax_invalidate(e);
  return mach_bare_call_finish(e, in);
}

static bool mach_load_home_preg(nyr_x64_mach_enc_t *e, int off, unsigned preg) {
  if (e->last_home_store_end != 0 && e->last_home_store_end == e->code.len &&
      e->last_home_store_off == off) {
    if (!mach_mov_preg_preg(e, (int)preg, e->last_home_store_preg))
      return false;
  } else {
    if (!mach_rex(e, true, preg >= 8, false, false) || !mach_u8(e, 0x8b) ||
        !mach_rbp_disp(e, preg, off))
      return false;
    if (!e->home_store_counts)
      e->home_load_ops++;
  }
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
  if (!mach_load_rax_slot(e, off, (int)op->as.reg))
    return false;
  e->last_rax_valid = true;
  e->last_rax_off = off;
  e->last_rax_vreg = (int)op->as.reg;
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
  /*
   * Eliding the LEA is valid only if this is its sole use. In particular,
   * load/store pairs need the address value to remain materialized between
   * the two operations.
   */
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

/*
 * Rebuilding an address at a later use re-reads the LEA's inputs. Their live
 * ranges may have ended at the LEA, leaving their spill homes and dedicated
 * loop registers free for values defined in between, so reject any overlap.
 */
static bool mach_sib_inputs_stable(const nyr_x64_mach_enc_t *e,
                                   const nyr_mach_inst_t *def, size_t def_index,
                                   size_t use_index) {
  const nyr_mach_operand_t *inputs[] = {&def->src0, &def->src1};
  bool same_block =
      mach_block_for_inst(e, def_index) == mach_block_for_inst(e, use_index);
  for (size_t n = 0; n < 2; ++n) {
    if (inputs[n]->kind != NYR_MACH_OPERAND_VREG)
      continue;
    uint32_t input = inputs[n]->as.reg;
    /*
     * The scan below is linear, so it is only exact inside one block. A
     * multiply-defined input could change on a path it cannot see.
     */
    if (!same_block && e->def_count && input < e->mach->vreg_len &&
        e->def_count[input] > 1)
      return false;
    /*
     * An input produced by the instruction just before the LEA may exist only
     * in RAX, with no home write to re-read later.
     */
    if (def_index > 0 && e->def_inst && input < e->mach->vreg_len &&
        e->def_inst[input] == def_index - 1)
      return false;
    int home = mach_vreg_home_off(e, input);
    for (size_t i = def_index; i < use_index; ++i) {
      const nyr_mach_inst_t *at = &e->mach->insts[i];
      if (at->opcode == NYR_MACH_STORE || at->dst.kind != NYR_MACH_OPERAND_VREG)
        continue;
      /*
       * Redefined after the address was formed, including `add v, v, x`.
       */
      if (at->dst.as.reg == input)
        return false;
      uint32_t other = at->dst.as.reg;
      if (mach_vreg_home_off(e, other) == home)
        return false;
      /*
       * Dedicated loop registers have the same hazard as shared homes.
       */
      if (e->loop_gpr && input < e->mach->vreg_len &&
          other < e->mach->vreg_len && e->loop_gpr[input] >= 0 &&
          e->loop_gpr[input] == e->loop_gpr[other])
        return false;
      /*
       * An ordinary allocator segment may also take the loop register once
       * the input's own live range (which ends at the LEA) is over. A loop
       * register has no published home, so the rebuilt address would read
       * the clobbered register or an unwritten slot.
       */
      if (e->loop_gpr && input < e->mach->vreg_len &&
          other < e->mach->vreg_len && e->loop_gpr[input] >= 0) {
        const nyr_mach_live_segment_t *seg = mach_segment_at(e, other, i);
        if (seg && seg->color == e->loop_gpr[input])
          return false;
      }
    }
  }
  return true;
}

/*
 * Decompose an indexed LEA at a memory use even when the address also feeds
 * another memory operation (for example, the load and store of an array
 * element swap).  Unlike mach_sib_lea_mem(), this never licenses deleting or
 * moving the LEA: it only lets this one access encode its address as SIB.
 */
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
      (!is_load && !is_store) ||
      !mach_sib_inputs_stable(e, def, def_index, use_index))
    return false;
  if (base_out)
    *base_out = &def->src0;
  if (index_out)
    *index_out = &def->src1;
  if (scale_out)
    *scale_out = (unsigned)def->src2.as.imm;
  return true;
}

/*
 * A multi-use address LEA may be omitted only if every use is itself a
 * memory address in this block.  Each access can then reconstruct the same
 * SIB address directly from the LEA's SSA inputs.
 */
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
    if ((!is_load && !is_store) || mach_block_for_inst(e, i) != block ||
        !mach_sib_inputs_stable(e, &e->mach->insts[def_index], def_index, i))
      return false;
    found = true;
  }
  return found;
}

/*
 * The scan below ran from each LEA candidate to the end of the function
 * (quadratic on large bodies). The machine form is const while encoding, so
 * one pass records each vreg's last float memory use.
 */
static const size_t *mach_float_mem_last(const nyr_x64_mach_enc_t *ce) {
  nyr_x64_mach_enc_t *e = (nyr_x64_mach_enc_t *)ce;
  const nyr_mach_func_t *m = e->mach;
  if (e->float_mem_last && e->float_mem_insts == m->insts &&
      e->float_mem_len == m->inst_len)
    return e->float_mem_last;
  free(e->float_mem_last);
  e->float_mem_last = calloc(m->vreg_len ? m->vreg_len : 1, sizeof(size_t));
  e->float_mem_insts = m->insts;
  e->float_mem_len = m->inst_len;
  if (!e->float_mem_last)
    return NULL;
  for (size_t i = 0; i < m->inst_len; ++i) {
    const nyr_mach_inst_t *in = &m->insts[i];
    if (in->opcode == NYR_MACH_LOAD && in->src0.kind == NYR_MACH_OPERAND_VREG &&
        in->src0.as.reg < m->vreg_len && mach_is_float(m, &in->dst))
      e->float_mem_last[in->src0.as.reg] = i + 1;
    if (in->opcode == NYR_MACH_STORE && in->dst.kind == NYR_MACH_OPERAND_VREG &&
        in->dst.as.reg < m->vreg_len && mach_is_float(m, &in->src0))
      e->float_mem_last[in->dst.as.reg] = i + 1;
  }
  return e->float_mem_last;
}

static bool mach_sib_lea_has_float_memory_use(const nyr_x64_mach_enc_t *e,
                                              uint32_t vreg, size_t def_index) {
  if (!e || !e->mach || vreg >= e->mach->vreg_len ||
      def_index >= e->mach->inst_len)
    return false;
  const size_t *last = mach_float_mem_last(e);
  if (last)
    return last[vreg] > def_index + 1;
  for (size_t i = def_index + 1; i < e->mach->inst_len; ++i) {
    const nyr_mach_inst_t *in = &e->mach->insts[i];
    if (!mach_inst_uses_vreg(in, vreg))
      continue;
    if (in->opcode == NYR_MACH_LOAD && mach_op_is_vreg(&in->src0, vreg) &&
        mach_is_float(e->mach, &in->dst))
      return true;
    if (in->opcode == NYR_MACH_STORE && mach_op_is_vreg(&in->dst, vreg) &&
        mach_is_float(e->mach, &in->src0))
      return true;
  }
  return false;
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
  /*
   * Folding `x << k` into the SIB scale reads x at this ADD, a use the
   * register allocator never saw (it saw the shifted value). If x has no
   * later reader it is dead here: its register is reused and its shared
   * home holds another value (revcomp -O2 loaded an index from a home that
   * was written only later and faulted). Fold only while x stays live.
   */
  if (scaled_value && index->kind == NYR_MACH_OPERAND_VREG) {
    bool live = false;
    size_t add_index = use_index > 0 ? use_index - 1 : use_index;
    for (size_t i = add_index; i < e->mach->inst_len && !live; ++i) {
      const nyr_mach_inst_t *later = &e->mach->insts[i];
      live = mach_inst_input_uses_vreg(later, index->as.reg) != 0 ||
             (later->opcode == NYR_MACH_STORE &&
              mach_op_is_vreg(&later->dst, index->as.reg));
    }
    if (!live)
      return false;
  }
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
  /*
   * O0's stack-only path does not keep the transient index/base pair live
   * across every inserted scalar conversion and store. Materialize the full
   * address there; SIB folding remains enabled once register allocation can
   * prove both operands resident.
   */
  if ((!e->fp_fast_path || e->force_stack_reads) &&
      e->cur_inst < e->mach->inst_len) {
    const nyr_mach_inst_t *use = &e->mach->insts[e->cur_inst];
    bool float_memory_use =
        (use->opcode == NYR_MACH_STORE && mach_is_float(e->mach, &use->src0)) ||
        (use->opcode == NYR_MACH_LOAD && mach_is_float(e->mach, &use->dst));
    if (float_memory_use)
      return false;
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
    /*
     * Only consume values whose current segments are already resident.
     * A skipped LEA can outlive either input's segment, in which case its
     * canonical-home fallback below is still required. R12 is a valid SIB
     * index with REX.X set; only RSP (register 4) means no index.
     */
    if (bp >= 0 && ip >= 0 && ip != 4) {
      *base_preg = (unsigned)bp;
      *idx_preg = (unsigned)ip;
      *scale = lea_scale;
      return true;
    }
    const nyr_mach_inst_t *use =
        e->cur_inst < e->mach->inst_len ? &e->mach->insts[e->cur_inst] : NULL;
    bool float_memory_use =
        use &&
        ((use->opcode == NYR_MACH_STORE &&
          mach_is_float(e->mach, &use->src0)) ||
         (use->opcode == NYR_MACH_LOAD && mach_is_float(e->mach, &use->dst)));
    if (float_memory_use &&
        (e->force_stack_reads || bp < 0 || ip < 0 || (ip & 7) == 4))
      return false;
    /*
     * A resident index needs no scratch: only the base goes through RAX.
     */
    if (ip >= 0 && ip != 4 && ip != 10) {
      if (!mach_seed_colored_preg(e, lea_index->as.reg) ||
          !mach_load_op_rax(e, lea_base))
        return false;
      *base_preg = 0;
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
      d->dst.as.reg != addr->as.reg ||
      !mach_sib_inputs_stable(e, d, di, e->cur_inst))
    return false;
  /*
   * def_inst holds a vreg's last definition, which need not be the one
   * reaching this access once the vreg has several.
   */
  if (e->def_count && e->def_count[addr->as.reg] > 1 &&
      (di >= e->cur_inst ||
       mach_block_for_inst(e, di) != mach_block_for_inst(e, e->cur_inst)))
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
      if ((e->def_count && e->def_count[s->as.reg] > 1) ||
          !mach_sib_inputs_stable(e, &e->mach->insts[si], si, e->cur_inst))
        return false;
      iop = &e->mach->insts[si].src0;
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

  const nyr_mach_inst_t *use =
      e->cur_inst < e->mach->inst_len ? &e->mach->insts[e->cur_inst] : NULL;
  bool float_memory_use =
      use &&
      ((use->opcode == NYR_MACH_STORE && mach_is_float(e->mach, &use->src0)) ||
       (use->opcode == NYR_MACH_LOAD && mach_is_float(e->mach, &use->dst)));
  if (float_memory_use &&
      (e->force_stack_reads || bp < 0 || ip < 0 || (ip & 7) == 4))
    return false;

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
  int resident_index = mach_vreg_preg(e, add_index->as.reg);
  if (resident_index >= 0 && resident_index != 4 && resident_index != 10) {
    if (!mach_seed_colored_preg(e, add_index->as.reg) ||
        !mach_load_op_rax(e, add_base))
      return false;
    *base_preg = 0;
    *idx_preg = (unsigned)resident_index;
    *scale = add_scale;
    return true;
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
  /*
   * A store reads its address and value without clobbering either register.
   * Use a resident value directly, including when it is also an address
   * operand; moving it to a scratch would only introduce copies and spills.
   */
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
static bool mach_vex(nyr_x64_mach_enc_t *e, unsigned pp, bool w, unsigned reg,
                     unsigned index, unsigned rm, unsigned vvvv, unsigned opc);

/*
 * Prefix + opcode of a scalar SSE move (movsd/movss 0F 10/11) with a
 * resolved memory operand: VEX on AVX targets (see mach_vex), legacy
 * F2/F3 REX 0F otherwise. The caller emits ModRM/SIB/disp.
 */
static bool mach_sse_mem_prefix(nyr_x64_mach_enc_t *e, bool f32,
                                const ny_x64_mem_addr_t *a, unsigned reg,
                                unsigned opc) {
  if (e->vex)
    return mach_vex(e, f32 ? 2 : 3, false, reg,
                    a->index >= 0 ? (unsigned)a->index : 0, a->base, 0, opc);
  return mach_u8(e, f32 ? 0xf3 : 0xf2) &&
         mach_rex_for_addr(e, a, false, reg >= 8) && mach_u8(e, 0x0f) &&
         mach_u8(e, opc);
}

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
  /*
   * Under a SIB byte only an RBP/R13 base lacks a mod=00 form; RSP/R12 bases
   * are encoded by the SIB itself. A redundant disp8 turns a simple
   * base+index LEA into a slower three-component one on Zen and Intel.
   */
  unsigned mod = a->disp == 0
                     ? ((a->base & 7) == 5 ? 1u : 0u)
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
  /*
   * The reg field is three bits; the caller's REX.R carries the high bit. A
   * caller that passes the full preg (mach_try_fuse_load_add folded a load
   * into `add r14, [rax]`) must not leak preg 8..15 into the mod bits: reg 14
   * produced 0x70, i.e. "disp8 follows", with no displacement byte emitted,
   * so the next instruction's REX prefix was decoded as that displacement and
   * its own operation ran without it (currying's `add $3,%r13` became
   * `add $3,%ebp`, clobbering the frame pointer).
   */
  reg &= 7;
  if ((base & 7) == 4)
    return mach_u8(e, (unsigned char)((reg << 3) | 0x04)) && mach_u8(e, 0x24);
  if ((base & 7) == 5)
    return mach_u8(e, (unsigned char)(0x40 | (reg << 3) | (base & 7))) &&
           mach_u8(e, 0x00);
  return mach_u8(e, (unsigned char)((reg << 3) | (base & 7)));
}

static bool mach_add_patch_kind(nyr_x64_mach_enc_t *e, uint32_t block,
                                bool rel8) {
  if (e->patch_count == e->patch_cap) {
    size_t nc = e->patch_cap ? e->patch_cap * 2 : 32;
    size_t *na = realloc(e->patch_at, nc * sizeof(*na));
    uint32_t *nb = realloc(e->patch_block, nc * sizeof(*nb));
    unsigned char *ns = realloc(e->patch_short, nc * sizeof(*ns));
    if (na)
      e->patch_at = na;
    if (nb)
      e->patch_block = nb;
    if (ns)
      e->patch_short = ns;
    if (!na || !nb || !ns)
      return mach_err(e, "x64 machine form encode: patch OOM");
    e->patch_cap = nc;
  }
  e->patch_at[e->patch_count] = e->code.len;
  e->patch_block[e->patch_count] = block;
  e->patch_short[e->patch_count] = rel8 ? 1 : 0;
  e->patch_count++;
  return true;
}

static bool mach_add_patch(nyr_x64_mach_enc_t *e, uint32_t block) {
  return mach_add_patch_kind(e, block, false);
}

static bool mach_add_patch_short(nyr_x64_mach_enc_t *e, uint32_t block) {
  return mach_add_patch_kind(e, block, true);
}

/*
 * Emit a branch to `block`. A backward edge already knows its target offset,
 * so when the displacement fits a signed byte emit the short form: two bytes
 * instead of five (jmp) or six (jcc). Forward edges keep the rel32 placeholder.
 *
 * Short branches still record a patch, flagged in patch_short, so the
 * trampoline fold can move them with the rest of the code before patching.
 * That fold only ever drops bytes, which can only shrink the magnitude of a
 * backward displacement, so a rel8 edge stays in range.
 *
 * jcc is 0 for an unconditional jump, otherwise the 0F 8x opcode byte.
 */
static bool mach_emit_branch_to(nyr_x64_mach_enc_t *e, uint32_t block,
                                unsigned char jcc) {
  if (!e || !e->mach || !e->block_off)
    return false;
  /*
   * Opt-out so both layouts can be measured from one binary.
   */
  const char *no_short = getenv("NYTRIX_NO_SHORT_BRANCHES");
  bool short_ok =
      !(no_short && strcmp(no_short, "0") != 0 && strcmp(no_short, "off") != 0);
  if (short_ok && e->cur_block_valid && block < e->cur_block &&
      block < e->mach->block_len) {
    int64_t rel = (int64_t)e->block_off[block] - (int64_t)(e->code.len + 2);
    if (rel >= -128 && rel <= 127) {
      unsigned char op = jcc ? (unsigned char)(0x70 | (jcc & 0x0f)) : 0xeb;
      return mach_u8(e, op) && mach_add_patch_short(e, block) &&
             mach_u8(e, 0x00);
    }
  }
  if (jcc) {
    if (!mach_u8(e, 0x0f) || !mach_u8(e, jcc))
      return false;
  } else if (!mach_u8(e, 0xe9)) {
    return false;
  }
  return mach_add_patch(e, block) && mach_i32(e, 0);
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
  /*
   * C extern references carry an internal prefix; link against the ABI name.
   */
  static const char c_extern_prefix[] = "__nyc_extern_";
  if (sym && strncmp(sym, c_extern_prefix, sizeof(c_extern_prefix) - 1) == 0)
    sym += sizeof(c_extern_prefix) - 1;
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
 * R10 is reserved scratch, but adjacent scalar floating operations do not
 * clobber it. Preserve a cached indexed-load induction value across only
 * those instructions; every other operation invalidates the cache
 * conservatively.
 */
static bool mach_inst_preserves_r10_index(const nyr_x64_mach_enc_t *e,
                                          const nyr_mach_inst_t *in) {
  /*
   * An instruction may otherwise look like an adjacent LEA/ADD or a scalar
   * floating operation while redefining the very index cached in R10. The
   * next SIB access must reload that new value from its home/register.
   */
  if (e->r10_index_valid && in->dst.kind == NYR_MACH_OPERAND_VREG &&
      in->dst.as.reg == e->r10_index_vreg)
    return false;
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
static void mach_note_single_use(uint8_t *counts, size_t *sole_user,
                                 size_t vreg_len, const nyr_mach_operand_t *op,
                                 size_t pc) {
  if (op->kind != NYR_MACH_OPERAND_VREG || op->as.reg >= vreg_len)
    return;
  size_t v = op->as.reg;
  if (counts[v] < 2) {
    if (++counts[v] == 1)
      sole_user[v] = pc;
  }
}

static bool mach_prepare_select_bit_test(nyr_x64_mach_enc_t *e,
                                         const char *name) {
  if (!e || !e->mach || !e->select_test_src || !e->select_test_equal ||
      !e->cmp_elision_action || !e->select_skip_alu || !e->def_inst)
    return false;
  const nyr_mach_func_t *m = e->mach;
  size_t n = m->vreg_len;
  if (!n || n > SIZE_MAX / sizeof(size_t))
    return n == 0;
  bool has_candidate = false;
  for (size_t i = 0; i < m->inst_len; ++i) {
    const nyr_mach_inst_t *in = &m->insts[i];
    if (in->opcode == NYR_MACH_SELECT &&
        in->src0.kind == NYR_MACH_OPERAND_VREG && in->src0.as.reg < n &&
        (in->condition == NYR_MACH_COND_EQ ||
         in->condition == NYR_MACH_COND_NE)) {
      has_candidate = true;
      break;
    }
  }
  if (!has_candidate)
    return true;
  uint8_t *use_count = calloc(n, sizeof(*use_count));
  size_t *sole_user = malloc(n * sizeof(*sole_user));
  if (!use_count || !sole_user) {
    free(use_count);
    free(sole_user);
    return false;
  }
  for (size_t pc = 0; pc < m->inst_len; ++pc) {
    const nyr_mach_inst_t *in = &m->insts[pc];
    mach_note_single_use(use_count, sole_user, n, &in->src0, pc);
    mach_note_single_use(use_count, sole_user, n, &in->src1, pc);
    mach_note_single_use(use_count, sole_user, n, &in->src2, pc);
    for (size_t a = 0; a < in->args_len; ++a)
      mach_note_single_use(use_count, sole_user, n, &in->args[a], pc);
  }
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
    const nyr_mach_operand_t *negative =
        select->condition == NYR_MACH_COND_NE ? &select->src1 : &select->src2;
    const nyr_mach_operand_t *original =
        select->condition == NYR_MACH_COND_NE ? &select->src2 : &select->src1;
    int64_t zero;
    if (e->select_cmp_inst && cmp->opcode == NYR_MACH_CMP &&
        cmp->condition == NYR_MACH_COND_LT &&
        original->kind == NYR_MACH_OPERAND_VREG &&
        original->reg_class == NYR_MACH_REGCLASS_GPR &&
        mach_op_is_vreg(&cmp->src0, original->as.reg) &&
        mach_operand_const_i64(e, &cmp->src1, &zero) && zero == 0 &&
        negative->kind == NYR_MACH_OPERAND_VREG && negative->as.reg < n &&
        use_count[cond] == 1 && sole_user[cond] == si &&
        e->def_count[cond] == 1 && select->dst.kind == NYR_MACH_OPERAND_VREG &&
        mach_block_for_inst(e, ci) == mach_block_for_inst(e, si) &&
        use_count[negative->as.reg] == 1 &&
        e->def_count[negative->as.reg] == 1) {
      size_t ni = e->def_inst[negative->as.reg];
      if (ni < si && mach_block_for_inst(e, ni) == mach_block_for_inst(e, si)) {
        const nyr_mach_inst_t *neg = &m->insts[ni];
        if (neg->opcode == NYR_MACH_SUB && !neg->narrow32 &&
            mach_op_is_vreg(&neg->src1, original->as.reg) &&
            mach_operand_const_i64(e, &neg->src0, &zero) && zero == 0) {
          e->select_cmp_inst[cond] = ni;
          e->select_skip_alu[negative->as.reg] = 1;
          e->cmp_elision_action[cond] = MACH_CMP_SKIP_FOR_SELECT;
          SELECT_FUSE_TRACE("wrapping-abs");
          continue;
        }
      }
    }
    /*
     * Integer compare whose only user is this select: re-emit it at the
     * select as cmp + cmovcc. Its operands must still be live there; the
     * arms must be registers so the cmov has a source.
     */
    if (e->select_cmp_inst && !getenv("NYTRIX_NO_SELECT_CMP_FUSE") &&
        cmp->opcode == NYR_MACH_CMP && mach_cmov_cc(cmp->condition) >= 0 &&
        cmp->src0.kind == NYR_MACH_OPERAND_VREG &&
        cmp->src0.reg_class == NYR_MACH_REGCLASS_GPR &&
        cmp->src0.as.reg < m->vreg_len &&
        mach_select_cmp_rhs_ok(e, &cmp->src1, si) &&
        select->dst.kind == NYR_MACH_OPERAND_VREG &&
        mach_block_for_inst(e, ci) == mach_block_for_inst(e, si) &&
        use_count[cond] == 1 && sole_user[cond] == si &&
        mach_vreg_colored_at(e, cmp->src0.as.reg, si) &&
        mach_select_cmp_operand_live(e, select, cmp->src0.as.reg, si) &&
        (cmp->src1.kind != NYR_MACH_OPERAND_VREG ||
         mach_select_cmp_rhs_imm(e, &cmp->src1, &(int64_t){0}) ||
         mach_select_cmp_operand_live(e, select, cmp->src1.as.reg, si)) &&
        mach_select_arm_ok(e, &select->src1, si) &&
        mach_select_arm_ok(e, &select->src2, si) &&
        /*
         * Two constant arms have no register to compare-select between; the
         * generic path is correct there and the select should already have
         * folded. Keep fusion to at most one materialized arm.
         */
        !(mach_select_cmp_rhs_imm(e, &select->src1, &(int64_t){0}) &&
          mach_select_cmp_rhs_imm(e, &select->src2, &(int64_t){0})) &&
        mach_vreg_colored_at(e, select->dst.as.reg, si)) {
      e->select_cmp_inst[cond] = ci;
      e->select_test_equal[cond] = select->condition == NYR_MACH_COND_EQ;
      e->cmp_elision_action[cond] = MACH_CMP_SKIP_FOR_SELECT;
      SELECT_FUSE_TRACE("cmp-accepted");
      continue;
    }
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
    if (use_count[and_v] != 1 || sole_user[and_v] != ci ||
        use_count[cond] != 1 || sole_user[cond] != si) {
      SELECT_FUSE_TRACE("tree-not-single-use");
      continue;
    }
    /*
     * Fusion moves the source read from AND to SELECT, after allocation and
     * home coloring. Its original live range must already cover SELECT;
     * otherwise an intervening instruction may reuse its register or home.
     */
    if (!nyr_mach_regalloc_segment_at(&e->regalloc, source, si)) {
      SELECT_FUSE_TRACE("source-not-live-at-select");
      continue;
    }
    /*
     * The allocator segment can outlast the value's real last use (the AND),
     * letting a later definition share its register before the select: a
     * loop increment scheduled ahead of the select then got overwritten by
     * the reloaded test source (gcbench ran past its retained buffer).
     * Require an actual use at or after the select.
     */
    if (!mach_select_cmp_operand_live(e, select, source, si)) {
      SELECT_FUSE_TRACE("source-dead-before-select");
      continue;
    }
    e->select_test_src[cond] = (int32_t)source;
    e->select_test_equal[cond] = (cmp->condition == NYR_MACH_COND_EQ) !=
                                 (select->condition == NYR_MACH_COND_EQ);
    e->cmp_elision_action[cond] = MACH_CMP_SKIP_FOR_SELECT;
    e->select_skip_alu[and_v] = 1;
    SELECT_FUSE_TRACE("accepted");
  }
#undef SELECT_FUSE_TRACE

  free(use_count);
  free(sole_user);
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
  case NYR_MACH_SHR:
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
      /*
       * An indexed LEA folds a literal base into its displacement, and its
       * fallback rematerializes the literal, so the base needs no home.
       */
      bool lea_literal_base = oi == 0 && in->opcode == NYR_MACH_LEA &&
                              in->src1.kind == NYR_MACH_OPERAND_VREG &&
                              in->src1.as.reg != op->as.reg &&
                              in->src2.kind == NYR_MACH_OPERAND_IMM &&
                              op->kind == NYR_MACH_OPERAND_VREG &&
                              op->as.reg < vregs &&
                              e->imm_fold[op->as.reg] >= INT32_MIN &&
                              e->imm_fold[op->as.reg] <= INT32_MAX;
      if (op->kind == NYR_MACH_OPERAND_VREG && op->as.reg < vregs &&
          e->vreg_literal_rhs_only[op->as.reg] && !lea_literal_base &&
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
    /*
     * Flags reuse is detected but not accepted: the allocation view it needs
     * is rejected by the machine verifier (operand-less cmp/br_if), which
     * sent every function that matched to the stack encoder, and emitting the
     * reuse without that verification miscompiled `splay`. Keep the second
     * compare until the reuse path has a regression fixture.
     */
    if (getenv("NY_X64_CMP_REUSE") &&
        e->cmp_elision_action[cmp_reg] == MACH_CMP_KEEP) {
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
    /*
     * A shift by an immediate loads src0 through the same register-aware
     * materializer as ADD; only variable counts need RCX.
     */
    if ((in->opcode == NYR_MACH_SHL || in->opcode == NYR_MACH_SAR ||
         in->opcode == NYR_MACH_SHR) &&
        in->src1.kind == NYR_MACH_OPERAND_IMM &&
        !getenv("NYTRIX_SHIFT_SRC_HOME"))
      return false;
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
    case NYR_MACH_SHR:
    case NYR_MACH_ROR:
    case NYR_MACH_ROR32:
    case NYR_MACH_VSHR:
    case NYR_MACH_BSWAP64:
    case NYR_MACH_BSWAP32:
    case NYR_MACH_RET:
      /*
       * Return values are consumed after the producer's segment bookkeeping;
       * keep the canonical home populated unless the same-block final-use
       * proof confirms the assigned register remains available through RET.
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
 * True when `in` (an ADD/LEA) computes an address that some later memory
 * access in the same block consumes. The SIB rematerializer tolerates
 * intervening instructions, so an address ADD and its access need not be
 * adjacent: susan's stencil store follows the `mod` that produces the stored
 * value. `m` supplies the block bounds; the scan stops at the block end
 * because an address never survives a CFG edge as an address.
 */
static bool mach_inst_feeds_block_address(const nyr_mach_func_t *m,
                                          const nyr_mach_inst_t *in) {
  if (!m || !in || in->dst.kind != NYR_MACH_OPERAND_VREG)
    return false;
  if (in->opcode != NYR_MACH_ADD && in->opcode != NYR_MACH_LEA)
    return false;
  size_t at = (size_t)(in - m->insts);
  if (at >= m->inst_len)
    return false;
  size_t end = m->inst_len;
  for (size_t bi = 0; bi < m->block_len; ++bi) {
    const nyr_mach_block_t *b = &m->blocks[bi];
    size_t bend = b->first_inst + b->inst_count;
    if (at >= b->first_inst && at < bend) {
      end = bend < m->inst_len ? bend : m->inst_len;
      break;
    }
  }
  for (size_t i = at + 1; i < end; ++i) {
    const nyr_mach_inst_t *use = &m->insts[i];
    if ((use->opcode == NYR_MACH_LOAD &&
         mach_op_is_vreg(&use->src0, in->dst.as.reg)) ||
        (use->opcode == NYR_MACH_STORE &&
         mach_op_is_vreg(&use->dst, in->dst.as.reg)))
      return true;
  }
  return false;
}

/*
 * True when `vreg` feeds a memory address later in this block, either
 * directly or as an operand of the address ADD/LEA. The base-only path and the
 * SIB rematerializer both read such a value from its canonical home when it is
 * not resident, so it must be published even though its use would otherwise
 * qualify for the feed-from-color shortcut.
 */
static bool mach_vreg_feeds_block_address(const nyr_x64_mach_enc_t *e,
                                          uint32_t vreg) {
  if (!e || !e->mach || vreg >= e->mach->vreg_len)
    return false;
  size_t block = mach_block_for_inst(e, e->cur_inst);
  if (block == SIZE_MAX || block >= e->mach->block_len)
    return false;
  const nyr_mach_block_t *b = &e->mach->blocks[block];
  size_t end = b->first_inst + b->inst_count;
  if (end > e->mach->inst_len)
    end = e->mach->inst_len;
  for (size_t i = e->cur_inst + 1; i < end; ++i) {
    const nyr_mach_inst_t *in = &e->mach->insts[i];
    if (!mach_inst_uses_vreg(in, vreg))
      continue;
    if ((in->opcode == NYR_MACH_LOAD && mach_op_is_vreg(&in->src0, vreg)) ||
        (in->opcode == NYR_MACH_STORE && mach_op_is_vreg(&in->dst, vreg)) ||
        mach_inst_feeds_block_address(e->mach, in))
      return true;
  }
  return false;
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
  if (mach_vreg_can_feed_from_color(e, vreg) &&
      !mach_vreg_feeds_block_address(e, vreg))
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
    if (!mach_inst_uses_vreg(in, vreg))
      continue;
    /*
     * Address operands are read from the canonical home whenever the access
     * cannot consume a register: the base-only path loads the address value
     * itself, and the SIB rematerializer loads each non-resident base or
     * index. That covers non-adjacent uses too, which is why an operand of an
     * address ADD that feeds the block must be classified here rather than by
     * the adjacency rule in mach_sib_add_mem. Without it the defining value
     * skipped its home write and the access indexed with uninitialized stack
     * (susan's stencil store read a never-written slot and wrote 0 for every
     * interior pixel).
     */
    bool address_use =
        (in->opcode == NYR_MACH_LOAD && mach_op_is_vreg(&in->src0, vreg)) ||
        (in->opcode == NYR_MACH_STORE && mach_op_is_vreg(&in->dst, vreg)) ||
        mach_inst_feeds_block_address(e->mach, in);
    if (!address_use && !mach_use_requires_home(e->mach, in, vreg))
      continue;
    if (!address_use && mach_same_block_alu_src1_uses_color(e, in, i, vreg))
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
static bool mach_vreg_can_forward_to_next_rax(nyr_x64_mach_enc_t *e,
                                              uint32_t vreg) {
  if (!e || !e->mach || vreg >= e->colors_len ||
      e->cur_inst + 1 >= e->mach->inst_len)
    return false;
  /*
   * The RAX tag identifies a frame offset, not its value. A consumer
   * staging into a physical register cannot trust that tag for a shared
   * home, so it needs the producer to publish the value there.
   */
  if (mach_vreg_home_shared(e, vreg))
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
      /*
       * A spilled RHS of a register-resident integer ADD is consumed from
       * RAX by the direct ALU path below. This is the common `load; add`
       * shape: the accumulator is src0 and the loaded value is src1. Keep
       * the load in RAX and skip its canonical home write.
       */
      bool direct_alu_src1 = use->opcode == NYR_MACH_ADD &&
                             use->dst.kind == NYR_MACH_OPERAND_VREG &&
                             use->src0.kind == NYR_MACH_OPERAND_VREG &&
                             mach_op_is_vreg(&use->src1, vreg) &&
                             mach_is_i64(e->mach, &use->dst) &&
                             mach_is_i64(e->mach, &use->src0) &&
                             mach_is_i64(e->mach, &use->src1) &&
                             mach_vreg_assigned_preg(e, use->dst.as.reg) >= 0 &&
                             mach_vreg_assigned_preg(e, use->src0.as.reg) >= 0;
      /*
       * The LEA encoder may seed a resident base first, so an index forwarded
       * through RAX is only safe when the base needs no materialization.
       */
      bool direct_lea_index =
          use->opcode == NYR_MACH_LEA &&
          mach_vreg_assigned_preg(e, use->src0.as.reg) < 0 &&
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
      if ((!direct_source && !direct_alu_src1 && !direct_lea_index &&
           !store_address) ||
          (mach_use_requires_home(e->mach, use, vreg) && !direct_lea_index &&
           !direct_alu_src1))
        return false;
      for (size_t later = i + 1; later < block_end; ++later)
        if (mach_inst_uses_vreg(&e->mach->insts[later], vreg))
          return false;
      return true;
    }
    /*
     * V256 ALU instructions use vector registers only, preserving RAX and
     * allowing its cached value to reach the later scalar LEA index use.
     */
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
 * Fuse a single-use scalar load into the immediately following in-place ADD:
 *
 *   v = ld.i64 [addr]
 *   acc = add acc, v
 *
 * becomes `add acc,[addr]`.  The load result has no canonical home write, so
 * this also avoids the RAX staging copy that the ordinary load path needs.
 */
static bool mach_try_fuse_load_add(nyr_x64_mach_enc_t *e,
                                   const nyr_mach_inst_t *load, bool *applied) {
  if (applied)
    *applied = false;
  if (!e || !e->mach || !load || load->opcode != NYR_MACH_LOAD ||
      load->dst.kind != NYR_MACH_OPERAND_VREG)
    return true;
  size_t next = e->cur_inst + 1;
  if (next >= e->mach->inst_len)
    return true;
  const nyr_mach_inst_t *add = &e->mach->insts[next];
  if (add->opcode != NYR_MACH_ADD || add->dst.kind != NYR_MACH_OPERAND_VREG ||
      add->src0.kind != NYR_MACH_OPERAND_VREG ||
      !mach_op_is_vreg(&add->src1, load->dst.as.reg) ||
      !mach_is_i64(e->mach, &add->dst) || !mach_is_i64(e->mach, &add->src0) ||
      !mach_is_i64(e->mach, &add->src1))
    return true;
  if (load->sext_load || load->byte_width ||
      (load->mem_size != 0 && load->mem_size != 8))
    return true;
  /*
   * This proof also checks that the loaded value dies at the ADD and that
   * the destination and accumulator are register-resident.  Requiring the
   * same preg keeps the fused operation in place without a copy.
   */
  if (!mach_vreg_can_forward_to_next_rax(e, load->dst.as.reg))
    return true;
  int dp = mach_vreg_assigned_preg(e, add->dst.as.reg);
  int ap = mach_vreg_assigned_preg(e, add->src0.as.reg);
  if (dp < 0 || dp == 0 || dp == 10 || ap != dp)
    return true;
  ny_x64_mem_addr_t addr;
  if (!mach_resolve_mem_addr(e, &load->src0, &addr))
    return false;
  if (!mach_rex_for_addr(e, &addr, true, dp >= 8) || !mach_u8(e, 0x03) ||
      !mach_emit_mem(e, &addr, (unsigned)dp))
    return false;
  /*
   * The ADD now defines its destination at the load position.  Publish a
   * canonical home unless the destination is a dedicated loop register;
   * the skipped instruction's own bookkeeping will not run.
   */
  if (e->color_seeded) {
    for (uint32_t v = 0; v < e->colors_len; ++v) {
      if (v == add->dst.as.reg || !e->color_seeded[v])
        continue;
      if (mach_vreg_preg(e, v) == dp)
        e->color_seeded[v] = false;
    }
  }
  bool loop_resident = e->loop_gpr && add->dst.as.reg < e->colors_len &&
                       e->loop_gpr[add->dst.as.reg] >= 0;
  if (!loop_resident &&
      !mach_store_preg_home(e, dp, mach_vreg_home_off(e, add->dst.as.reg)))
    return false;
  mach_rax_invalidate(e);
  if (e->color_seeded && add->dst.as.reg < e->colors_len)
    e->color_seeded[add->dst.as.reg] = true;
  e->cur_inst = next;
  if (applied)
    *applied = true;
  return true;
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
  for (uint32_t v = mach_next_set(e->color_seeded, 0, e->colors_len);
       v < e->colors_len;
       v = mach_next_set(e->color_seeded, v + 1, e->colors_len)) {
    /*
     * A carried segment elsewhere in the function is not evidence that this
     * block currently has the value in its physical register.  Synchronize
     * only an actually seeded resident value; otherwise a later segment can
     * cause an unrelated register to overwrite this vreg's canonical home.
     */
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
 * A spilled colored scalar can feed one same-block scalar operation or its
 * terminal return directly from its assigned preg when that is its final use.
 * Keep the home write for CFG edges, calls, fixed-register instructions, and
 * any gap that could reuse the color before the value is consumed.
 */
static bool mach_vreg_can_feed_from_color(const nyr_x64_mach_enc_t *e,
                                          uint32_t vreg) {
  if (!e || !e->mach || vreg >= e->colors_len || e->force_stack_reads ||
      mach_vreg_used_in_later_block(e, vreg))
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
  case NYR_MACH_MOD:
  case NYR_MACH_RET:
    break;
  default:
    return false;
  }
  bool direct_mod_source = use->opcode == NYR_MACH_MOD &&
                           !use->unsigned_arith &&
                           mach_op_is_vreg(&use->src0, vreg);
  if (direct_mod_source) {
    int preg = mach_color_preg[seg->color];
    /*
     * Constant-remainder lowering clobbers RAX, RCX, RDX, R8-R10. A source
     * in any of those registers must retain the normal home path; other
     * colors can feed the fixed-register sequence directly through RAX.
     */
    if (preg == 0 || preg == 1 || preg == 2 || preg == 8 || preg == 9 ||
        preg == 10)
      return false;
  }
  bool direct_ret =
      use->opcode == NYR_MACH_RET && mach_op_is_vreg(&use->src0, vreg);
  bool direct_select = false;
  if (use->opcode == NYR_MACH_SELECT &&
      use->src0.kind == NYR_MACH_OPERAND_VREG && e->select_test_src &&
      use->src0.as.reg < e->colors_len &&
      e->select_test_src[use->src0.as.reg] >= 0 &&
      (mach_op_is_vreg(&use->src1, vreg) ||
       mach_op_is_vreg(&use->src2, vreg))) {
    const nyr_mach_operand_t *ops[] = {&use->src1, &use->src2, &use->dst};
    direct_select = true;
    for (size_t k = 0; k < sizeof(ops) / sizeof(ops[0]); ++k) {
      const nyr_mach_live_segment_t *s =
          ops[k]->kind == NYR_MACH_OPERAND_VREG
              ? mach_segment_at(e, ops[k]->as.reg, use_idx)
              : NULL;
      if (!s || s->color < 0 || s->color >= MIR_COLOR_N)
        direct_select = false;
    }
  }
  if (!mach_inst_input_uses_vreg(use, vreg) ||
      (!direct_ret && !direct_mod_source && !direct_select &&
       mach_use_requires_home(e->mach, use, vreg)) ||
      mach_inst_has_preg(use))
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
 * vp* ymmD, ymmD, ymmS — VEX.256.66.0F.WIG opc /r.  The VEX.vvvv
 * source is the accumulator in ymmD; encoding it as ymm0 silently breaks
 * operations whenever allocation keeps the destination elsewhere.
 */
static bool mach_ymm_op_reg(nyr_x64_mach_enc_t *e, unsigned dst, unsigned src,
                            unsigned opc) {
  if (dst >= 16 || src >= 16)
    return mach_err(e, "x64: unsupported YMM register");
  unsigned char vex2 = (unsigned char)(0xe1u & ~(dst >= 8 ? 0x80u : 0u) &
                                       ~(src >= 8 ? 0x20u : 0u));
  unsigned char vex3 = (unsigned char)(0x05u | ((~dst & 0x0fu) << 3));
  return mach_u8(e, 0xc4) && mach_u8(e, vex2) && mach_u8(e, vex3) &&
         mach_u8(e, opc) && mach_u8(e, 0xc0 | ((dst & 7) << 3) | (src & 7));
}

/*
 * vp* ymmD, ymmA, ymmB — non-destructive VEX.256.66.0F.WIG opc /r. Both
 * sources are read before ymmD is written, so D may equal A or B.
 */
static bool mach_ymm_op3_reg(nyr_x64_mach_enc_t *e, unsigned dst, unsigned a,
                             unsigned b, unsigned opc) {
  if (dst >= 16 || a >= 16 || b >= 16)
    return mach_err(e, "x64: unsupported YMM register");
  unsigned char vex2 = (unsigned char)(0xe1u & ~(dst >= 8 ? 0x80u : 0u) &
                                       ~(b >= 8 ? 0x20u : 0u));
  unsigned char vex3 = (unsigned char)(0x05u | ((~a & 0x0fu) << 3));
  return mach_u8(e, 0xc4) && mach_u8(e, vex2) && mach_u8(e, vex3) &&
         mach_u8(e, opc) && mach_u8(e, 0xc0 | ((dst & 7) << 3) | (b & 7));
}

/*
 * vp* ymmD, ymmA, [rbp+off] — VEX.256.66.0F.WIG opc 85 disp32
 */
static bool mach_ymm_op3_mem(nyr_x64_mach_enc_t *e, unsigned dst, unsigned a,
                             int off, unsigned opc) {
  if (dst >= 16 || a >= 16)
    return mach_err(e, "x64: unsupported YMM register");
  unsigned char vex3 = (unsigned char)(0x05u | ((~a & 0x0fu) << 3));
  return mach_u8(e, 0xc4) && mach_u8(e, dst >= 8 ? 0x61 : 0xe1) &&
         mach_u8(e, vex3) && mach_u8(e, opc) &&
         mach_u8(e, 0x85 | ((dst & 7) << 3)) && mach_i32(e, off);
}

/*
 * vbroadcastsd ymmD, [rbp+off] — VEX.256.66.0F38.W0 19 /r. Four-lane f64
 * SET1: one memory broadcast replaces the scalar movddup used for V128.
 */
static bool mach_ymm_broadcastsd_mem(nyr_x64_mach_enc_t *e, unsigned reg,
                                     int off) {
  if (reg >= 16)
    return mach_err(e, "x64: unsupported YMM register");
  return mach_u8(e, 0xc4) && mach_u8(e, reg >= 8 ? 0x62 : 0xe2) &&
         mach_u8(e, 0x7d) && mach_u8(e, 0x19) &&
         mach_u8(e, 0x85 | ((reg & 7) << 3)) && mach_i32(e, off);
}

/*
 * vbroadcastsd ymmD, xmmS — VEX.256.66.0F38.W0 19 /r, register source
 * (AVX2). Avoids a store/broadcast-load round trip through the stack when
 * the scalar is already resident.
 */
static bool mach_ymm_broadcastsd_reg(nyr_x64_mach_enc_t *e, unsigned dst,
                                     unsigned src) {
  if (dst >= 16 || src >= 16)
    return mach_err(e, "x64: unsupported YMM register");
  unsigned char vex2 = (unsigned char)(0x02u | 0x40u | (dst >= 8 ? 0u : 0x80u) |
                                       (src >= 8 ? 0u : 0x20u));
  return mach_u8(e, 0xc4) && mach_u8(e, vex2) && mach_u8(e, 0x7d) &&
         mach_u8(e, 0x19) && mach_u8(e, 0xc0 | ((dst & 7) << 3) | (src & 7));
}

/*
 * AVX1 lacks the YMM vbroadcastsd form. Build four copies from one 64-bit
 * stack lane with a 128-bit shuffle and an AVX lane insert instead.
 */
static bool mach_ymm_set1_i64_avx_rax(nyr_x64_mach_enc_t *e) {
  return mach_u8(e, 0xc4) && mach_u8(e, 0xe1) && mach_u8(e, 0xf9) &&
         mach_u8(e, 0x6e) && mach_u8(e, 0xc0) && mach_u8(e, 0xc5) &&
         mach_u8(e, 0xf9) && mach_u8(e, 0x70) && mach_u8(e, 0xc0) &&
         mach_u8(e, 0x44) && mach_u8(e, 0xc4) && mach_u8(e, 0xe3) &&
         mach_u8(e, 0x7d) && mach_u8(e, 0x18) && mach_u8(e, 0xc0) &&
         mach_u8(e, 0x01);
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
 * vinsertf128 ymmD, ymmS, xmmR, imm — AVX lane assembly for SSE2 fallback.
 */
static bool mach_xmm_vinsertf128(nyr_x64_mach_enc_t *e, unsigned dst,
                                 unsigned src_ymm, unsigned src_xmm,
                                 unsigned half) {
  if (dst >= 16 || src_ymm >= 16 || src_xmm >= 16 || half > 1)
    return mach_err(e, "x64: unsupported vinsertf128 register");
  unsigned vex2 = 0xe3; /* ~R=1, ~X=1, ~B=1, map 0F3A */
  if (dst >= 8)
    vex2 &= ~0x80u;
  if (src_xmm >= 8)
    vex2 &= ~0x20u;
  unsigned char vex3 = (unsigned char)(0x05u | (((~src_ymm) & 0xfu) << 3));
  return mach_u8(e, 0xc4) && mach_u8(e, vex2) && mach_u8(e, vex3) &&
         mach_u8(e, 0x18) &&
         mach_u8(e, 0xc0u | ((dst & 7) << 3) | (src_xmm & 7)) &&
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
  if (dst == src)
    return true;
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
 * VEX prefix for a 0F-map instruction (pp: 0 none, 1 66, 2 F3, 3 F2).
 * `reg`/`index`/`rm` are the full register numbers behind ModRM.reg, SIB.index
 * and ModRM.rm/SIB.base (their bit 3 becomes VEX.R/X/B); `vvvv` is the extra
 * source register, or 0 for "none" (encoded 1111). Legacy SSE executed with
 * dirty upper YMM halves stalls on the host, so AVX targets encode scalar and
 * 128-bit SSE helpers this way; VEX.128 also zeroes bits 128-255.
 */
static bool mach_vex(nyr_x64_mach_enc_t *e, unsigned pp, bool w, unsigned reg,
                     unsigned index, unsigned rm, unsigned vvvv, unsigned opc) {
  unsigned r = reg >= 8, x = index >= 8, b = rm >= 8;
  unsigned v = (~vvvv) & 15u;
  if (!x && !b && !w)
    return mach_u8(e, 0xc5) &&
           mach_u8(e, ((r ? 0u : 1u) << 7) | (v << 3) | pp) && mach_u8(e, opc);
  return mach_u8(e, 0xc4) &&
         mach_u8(e, ((r ? 0u : 1u) << 7) | ((x ? 0u : 1u) << 6) |
                        ((b ? 0u : 1u) << 5) | 1u) &&
         mach_u8(e, ((w ? 1u : 0u) << 7) | (v << 3) | pp) && mach_u8(e, opc);
}

/*
 * movsd/movss xmmN <-> mem: F2/F3 REX 0F 10/11 modrm disp32.
 */
static bool mach_load_xmm(nyr_x64_mach_enc_t *e, int off, bool f32,
                          unsigned reg) {
  if (reg >= 16)
    return mach_err(e, "x64 machine form encode: unsupported XMM register");
  if (e->vex)
    return mach_vex(e, f32 ? 2 : 3, false, reg, 0, 5, 0, 0x10) &&
           mach_u8(e, 0x85 | ((reg & 7) << 3)) && mach_i32(e, off);
  return mach_u8(e, f32 ? 0xf3 : 0xf2) &&
         (reg >= 8 ? mach_rex(e, false, true, false, false) : true) &&
         mach_u8(e, 0x0f) && mach_u8(e, 0x10) &&
         mach_u8(e, 0x85 | ((reg & 7) << 3)) && mach_i32(e, off);
}

static bool mach_store_xmm(nyr_x64_mach_enc_t *e, int off, bool f32,
                           unsigned reg) {
  if (reg >= 16)
    return mach_err(e, "x64 machine form encode: unsupported XMM register");
  if (mach_home_store_skip(e, off))
    return true;
  if (e->vex)
    return mach_vex(e, f32 ? 2 : 3, false, reg, 0, 5, 0, 0x11) &&
           mach_u8(e, 0x85 | ((reg & 7) << 3)) && mach_i32(e, off);
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

/*
 * Scalar register copy. `movsd/movss xmm, xmm` merge into the destination's
 * upper lanes, so the copy waits for the destination's previous writer and
 * can serialize otherwise independent loop iterations (a divide whose
 * dividend copy depends on the last accumulate). Scalar values never keep
 * live data in the upper lanes, so copy the full register with `movaps`.
 */
static bool mach_sse_mov_reg(nyr_x64_mach_enc_t *e, unsigned dst, unsigned src,
                             bool f32) {
  (void)f32;
  if (dst >= 16 || src >= 16)
    return mach_err(e, "x64 machine form encode: unsupported XMM register");
  if (e->vex)
    return mach_vex(e, 0, false, dst, 0, src, 0, 0x28) &&
           mach_u8(e, (unsigned)(0xc0 | ((dst & 7) << 3) | (src & 7)));
  unsigned rex = 0x40 | (dst >= 8 ? 0x04 : 0) | (src >= 8 ? 0x01 : 0);
  return (rex != 0x40 ? mach_u8(e, rex) : true) && mach_u8(e, 0x0f) &&
         mach_u8(e, 0x28) &&
         mach_u8(e, (unsigned)(0xc0 | ((dst & 7) << 3) | (src & 7)));
}

static bool mach_sse_op_reg(nyr_x64_mach_enc_t *e, unsigned dst, unsigned src,
                            bool f32, unsigned opc) {
  if (dst >= 16 || src >= 16)
    return mach_err(e, "x64 machine form encode: unsupported XMM register");
  /*
   * Scalar arithmetic: VEX three-operand form with vvvv = dst keeps the
   * legacy upper-lane merge from the destination.
   */
  if (e->vex)
    return mach_vex(e, f32 ? 2 : 3, false, dst, 0, src, dst, opc) &&
           mach_u8(e, (unsigned)(0xc0 | ((dst & 7) << 3) | (src & 7)));
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
  if (e->vex)
    return mach_vex(e, f32 ? 2 : 3, false, dst, 0, 5, dst, opc) &&
           mach_u8(e, (unsigned)(0x85 | ((dst & 7) << 3))) && mach_i32(e, off);
  return mach_u8(e, f32 ? 0xf3 : 0xf2) &&
         (dst >= 8 ? mach_rex(e, false, true, false, false) : true) &&
         mach_u8(e, 0x0f) && mach_u8(e, opc) &&
         mach_u8(e, (unsigned)(0x85 | ((dst & 7) << 3))) && mach_i32(e, off);
}

static bool mach_sse_cmp_reg(nyr_x64_mach_enc_t *e, unsigned lhs, unsigned rhs,
                             bool f32) {
  if (lhs >= 16 || rhs >= 16)
    return mach_err(e, "x64 machine form encode: unsupported XMM register");
  if (e->vex)
    return mach_vex(e, f32 ? 0 : 1, false, lhs, 0, rhs, 0, 0x2e) &&
           mach_u8(e, (unsigned)(0xc0 | ((lhs & 7) << 3) | (rhs & 7)));
  unsigned rex = 0x40 | (lhs >= 8 ? 0x04 : 0) | (rhs >= 8 ? 0x01 : 0);
  return (f32 ? true : mach_u8(e, 0x66)) &&
         (rex != 0x40 ? mach_u8(e, rex) : true) && mach_u8(e, 0x0f) &&
         mach_u8(e, 0x2e) &&
         mach_u8(e, (unsigned)(0xc0 | ((lhs & 7) << 3) | (rhs & 7)));
}

static bool mach_mov_rax_xmm(nyr_x64_mach_enc_t *e, unsigned dst) {
  if (dst >= 16)
    return mach_err(e, "x64 machine form encode: unsupported XMM register");
  if (e->vex) /* vmovq xmm, rax: VEX.128.66.0F.W1 6E */
    return mach_vex(e, 1, true, dst, 0, 0, 0, 0x6e) &&
           mach_u8(e, (unsigned)(0xc0 | ((dst & 7) << 3)));
  return mach_u8(e, 0x66) && mach_rex(e, true, dst >= 8, false, false) &&
         mach_u8(e, 0x0f) && mach_u8(e, 0x6e) &&
         mach_u8(e, (unsigned)(0xc0 | ((dst & 7) << 3)));
}
static bool mach_mov_xmm_rax(nyr_x64_mach_enc_t *e, unsigned src) {
  if (src >= 16)
    return mach_err(e, "x64 machine form encode: unsupported XMM register");
  if (e->vex) /* vmovq rax, xmm: VEX.128.66.0F.W1 7E */
    return mach_vex(e, 1, true, src, 0, 0, 0, 0x7e) &&
           mach_u8(e, (unsigned)(0xc0 | ((src & 7) << 3)));
  return mach_u8(e, 0x66) && mach_rex(e, true, src >= 8, false, false) &&
         mach_u8(e, 0x0f) && mach_u8(e, 0x7e) &&
         mach_u8(e, (unsigned)(0xc0 | ((src & 7) << 3)));
}

static bool mach_float_begin_segments(nyr_x64_mach_enc_t *e) {
  if (!e || !e->fp_fast_path || !e->fp_color_seeded)
    return true;
  if (!e->fp_seg_start_offsets &&
      !mach_seg_index_build(&e->fp_regalloc, e->mach->inst_len, false,
                            &e->fp_seg_start_offsets, &e->fp_seg_start_vregs))
    return mach_err(e, "x64 machine form encode: OOM");
  if (e->cur_inst >= e->mach->inst_len)
    return true;
  for (uint32_t at = e->fp_seg_start_offsets[e->cur_inst];
       at < e->fp_seg_start_offsets[e->cur_inst + 1]; ++at) {
    uint32_t v = e->fp_seg_start_vregs[at];
    if (v >= e->fp_colors_len)
      continue;
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
  if (!e->fp_seg_end_offsets &&
      !mach_seg_index_build(&e->fp_regalloc, e->mach->inst_len, true,
                            &e->fp_seg_end_offsets, &e->fp_seg_end_vregs))
    return mach_err(e, "x64 machine form encode: OOM");
  if (e->cur_inst >= e->mach->inst_len)
    return true;
  for (uint32_t at = e->fp_seg_end_offsets[e->cur_inst];
       at < e->fp_seg_end_offsets[e->cur_inst + 1]; ++at) {
    uint32_t v = e->fp_seg_end_vregs[at];
    if (v >= e->fp_colors_len)
      continue;
    if (!e->fp_color_seeded[v])
      continue;
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
  if (!e->vec_seg_start_offsets &&
      !mach_seg_index_build(&e->vec_regalloc, e->mach->inst_len, false,
                            &e->vec_seg_start_offsets, &e->vec_seg_start_vregs))
    return mach_err(e, "x64 machine form encode: OOM");
  if (e->cur_inst >= e->mach->inst_len)
    return true;
  for (uint32_t at = e->vec_seg_start_offsets[e->cur_inst];
       at < e->vec_seg_start_offsets[e->cur_inst + 1]; ++at) {
    uint32_t v = e->vec_seg_start_vregs[at];
    if (v >= e->vec_colors_len)
      continue;
    if (!e->vec_color_seeded[v])
      continue;
    const nyr_mach_live_segment_t *seg = mach_vsegment_at(e, v, e->cur_inst);
    if (!seg || seg->start != e->cur_inst)
      continue;
    const nyr_mach_inst_t *in = &e->mach->insts[e->cur_inst];
    bool defines_value =
        in->dst.kind == NYR_MACH_OPERAND_VREG && in->dst.as.reg == v;
    if (e->vector_loop_carry_seeding && seg->carried && seg->color >= 0) {
      /*
       * A carried segment may also be marked spill because one successor
       * leaves the loop. Its common preg is still valid at block entry; the
       * end-of-block path publishes it to the home for that exit edge.
       */
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
  /*
   * Immutability needs a single definition. A loop-carried vector (a
   * reduction accumulator or any phi destination) is redefined every
   * iteration, so its home is stale until the exit publish writes it; the
   * exit-homing `spill` mark on its carried segment must then really store.
   * Skipping that store made `vec8.reduce_add` read an unwritten home after a
   * zero-trip vector loop (result += invariant over 3 iterations: 2491, not
   * 2451).
   */
  if (!e || !e->mach || !e->def_inst || !e->def_count || !e->cur_block_valid ||
      vreg >= e->vec_colors_len || vreg >= e->mach->vreg_len ||
      e->def_count[vreg] != 1 || e->cur_block >= e->mach->block_len)
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

/*
 * A single-definition vector whose home was stored in its defining block
 * after the definition: that block dominates every use, and nothing else
 * writes the vreg, so the home is current at every later program point.
 * Re-storing it at each loop latch (the common case for loop-invariant
 * broadcasts) is pure memory traffic.
 */
static bool mach_vector_home_stored_in_def_block(const nyr_x64_mach_enc_t *e,
                                                 uint32_t vreg) {
  if (!e || !e->vec_home_stored || !e->def_count || !e->def_inst ||
      vreg >= e->mach->vreg_len || e->def_count[vreg] != 1 ||
      !e->cur_block_valid)
    return false;
  size_t def = e->def_inst[vreg];
  size_t def_block =
      def < e->mach->inst_len ? mach_block_for_inst(e, def) : SIZE_MAX;
  if (def_block == SIZE_MAX || def_block == e->cur_block)
    return false;
  if (e->vec_home_stored[vreg])
    return true;
  /*
   * A defining segment that is uncolored or marked spill publishes the home
   * by its end (later segments reload from it). Segments never span blocks,
   * so that happens inside the defining block.
   */
  const nyr_mach_live_segment_t *dseg = mach_vsegment_at(e, vreg, def);
  return dseg && dseg->start == def && dseg->block == def_block &&
         (dseg->color < 0 || dseg->spill);
}

static void mach_vector_note_home_store(nyr_x64_mach_enc_t *e, uint32_t vreg) {
  if (!e || !e->vec_home_stored || !e->def_count || !e->def_inst ||
      vreg >= e->mach->vreg_len || e->def_count[vreg] != 1 ||
      !e->cur_block_valid)
    return;
  /*
   * Every caller stores after the definition was emitted (end-of-segment
   * stores run after their instruction, so def == cur_inst is fine).
   */
  size_t def = e->def_inst[vreg];
  if (def <= e->cur_inst && mach_block_for_inst(e, def) == e->cur_block)
    e->vec_home_stored[vreg] = true;
}

/*
 * Write a seeded vector register back to its stack home unless the home is
 * already current (see mach_vector_home_stored_in_def_block).
 */
static bool mach_vector_publish_home(nyr_x64_mach_enc_t *e, uint32_t v,
                                     const nyr_mach_live_segment_t *seg) {
  if (mach_vector_home_stored_in_def_block(e, v))
    return true;
  bool is_v256 = mach_vreg_is_v256(e->mach, v);
  if (!(is_v256 ? mach_ymm_store_v256(e, mach_vreg_home_off(e, v),
                                      mach_vec_color_preg(seg->color))
                : mach_store_xmm_v128(e, mach_vreg_home_off(e, v),
                                      mach_vec_color_preg(seg->color))))
    return false;
  mach_vector_note_home_store(e, v);
  return true;
}

/*
 * Before control leaves a single-definition vector's defining block, store
 * its register to the home once (1 = stored, 0 = not applicable, -1 = error).
 * The defining block dominates every use, so the home then stays current and
 * later flushes, typically one per loop latch for an invariant broadcast,
 * skip their store. Callers invoke this only where the emitted store runs on
 * every path out of the block (before the branch, or on fall-through).
 */
static int
mach_vector_publish_once_in_def_block(nyr_x64_mach_enc_t *e, uint32_t v,
                                      const nyr_mach_live_segment_t *seg) {
  if (!e->def_count || !e->def_inst || !e->vec_home_stored ||
      !e->cur_block_valid || v >= e->mach->vreg_len || e->def_count[v] != 1 ||
      e->vec_home_stored[v] || e->def_inst[v] >= e->cur_inst ||
      mach_block_for_inst(e, e->def_inst[v]) != e->cur_block ||
      !nyr_mach_regalloc_live_out(&e->vec_regalloc, e->cur_block, v))
    return 0;
  return mach_vector_publish_home(e, v, seg) ? 1 : -1;
}

static bool mach_vector_end_segments(nyr_x64_mach_enc_t *e) {
  if (!e || !e->vec_fast_path || !e->vec_color_seeded)
    return true;
  if (!e->vec_seg_end_offsets &&
      !mach_seg_index_build(&e->vec_regalloc, e->mach->inst_len, true,
                            &e->vec_seg_end_offsets, &e->vec_seg_end_vregs))
    return mach_err(e, "x64 machine form encode: OOM");
  if (e->cur_inst >= e->mach->inst_len)
    return true;
  for (uint32_t at = e->vec_seg_end_offsets[e->cur_inst];
       at < e->vec_seg_end_offsets[e->cur_inst + 1]; ++at) {
    uint32_t v = e->vec_seg_end_vregs[at];
    if (v >= e->vec_colors_len)
      continue;
    const nyr_mach_live_segment_t *seg = mach_vsegment_at(e, v, e->cur_inst);
    if (!seg || seg->end != e->cur_inst)
      continue;
    const nyr_mach_live_segment_t *next =
        mach_vsegment_at(e, v, e->cur_inst + 1);
    bool carried = e->vector_loop_carry_seeding && seg->carried && !seg->spill;
    bool needs_home =
        !carried &&
        (seg->spill || (next && !mach_vsegment_carries(e, v, next)));
    bool unchanged_spill_home =
        seg->spill && mach_vector_spill_home_is_current(e, v);
    if (needs_home && seg->color >= 0 && e->vec_color_seeded[v] &&
        !unchanged_spill_home && !mach_vector_publish_home(e, v, seg))
      return false;
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
  unsigned low = v;
  if (v256) {
    /*
     * VEXTRACTF128 is an AVX instruction and extracts bits without applying
     * floating-point semantics. VEXTRACTI128 would unnecessarily require
     * AVX2 for this reduction.
     *
     * Use VEX.128 on AVX targets even though the result fits an XMM.
     * A legacy paddq here repeatedly pays the dirty-upper transition when
     * a surrounding loop consumes YMM values.
     */
    unsigned high = v == 1 ? 0 : 1;
    if (e->vex) {
      if (!mach_xmm_vextractf128(e, high, v, 1) ||
          !mach_vex(e, 1, false, 0, 0, high, v, 0xd4) ||
          !mach_u8(e, 0xc0 | high))
        return false;
    } else {
      unsigned bottom = high ^ 1u;
      if (!mach_xmm_vextractf128(e, high, v, 1) ||
          !mach_xmm_vextractf128(e, bottom, v, 0) ||
          !mach_vec_op_reg(e, 0, 1, 0x66, 0xd4))
        return false;
    }
    low = 0;
  }
  if (e->vex) {
    unsigned shuffle = low == 1 ? 0 : 1;
    if (!mach_vex(e, 1, false, shuffle, 0, low, 0, 0x70) ||
        !mach_u8(e, 0xc0 | (shuffle << 3) | (low & 7)) || !mach_u8(e, 0x4e) ||
        !mach_vex(e, 1, false, 0, 0, shuffle, low, 0xd4) ||
        !mach_u8(e, 0xc0 | shuffle))
      return false;
  } else if ((low != 0 && !mach_vec_mov_reg(e, 0, low)) || !mach_u8(e, 0x66) ||
             !mach_u8(e, 0x0f) || !mach_u8(e, 0x70) || !mach_u8(e, 0xc8) ||
             !mach_u8(e, 0x4e) || !mach_vec_op_reg(e, 0, 1, 0x66, 0xd4)) {
    return false;
  }
  return mach_mov_xmm_rax(e, 0) && mach_rex(e, true, false, false, false) &&
         mach_u8(e, 0x01) && mach_u8(e, 0xd0) &&
         mach_commit_vreg(e, &in->dst, dst_off);
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

static bool mach_vector_load_single_use_reduction(const nyr_x64_mach_enc_t *e,
                                                  uint32_t vreg) {
  if (!e || !e->mach || vreg >= e->mach->vreg_len)
    return false;
  size_t block = mach_block_for_inst(e, e->cur_inst);
  for (size_t i = e->cur_inst + 1; i < e->mach->inst_len; ++i) {
    const nyr_mach_inst_t *next = &e->mach->insts[i];
    if (mach_block_for_inst(e, i) != block)
      return false;
    if (!mach_inst_uses_vreg(next, vreg))
      continue;
    return next->opcode == NYR_MACH_REDUCE_ADD &&
           next->src1.kind == NYR_MACH_OPERAND_VREG &&
           next->src1.as.reg == vreg;
  }
  return false;
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
    /*
     * Resolve through the shared address query like mach_vector_store: when
     * the producing ADD/LEA was elided into a pending SIB address, only the
     * query knows base/index. Loading the pointer's home into RAX instead
     * read a slot that was never written (bce-matrix at -O3 crashed in its
     * vectorized inner loop).
     */
    ny_x64_mem_addr_t addr;
    if (!mach_resolve_mem_addr(e, &in->src0, &addr) || !mach_u8(e, 0xf3) ||
        !mach_rex_for_addr(e, &addr, false, d >= 8) || !mach_u8(e, 0x0f) ||
        !mach_u8(e, 0x6f) || !mach_emit_mem(e, &addr, d & 7))
      return false;
  } else {
    return mach_err(e, "x64 machine form encode: vector load from pointer");
  }
  if (assigned >= 0) {
    /*
     * Later split/spilled segments need the canonical home. A vector pair
     * consumed only by the immediately following reduction stays resident in
     * its assigned XMM register, so publishing an otherwise unread home here
     * would add a stack store on every loop iteration.
     */
    if (!mach_vector_load_single_use_reduction(e, in->dst.as.reg) &&
        !mach_store_xmm_v128(e, dst_off, d))
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
 * AVX1 has 256-bit moves but no packed 64-bit integer add/sub. Split both
 * operands into XMM halves, use the SSE2 qword operation, then reassemble the
 * result. Keep the fallback in registers so a legal AVX-only target does not
 * round-trip each lane through scalar stack traffic.
 */
static bool mach_vector_ymm_i64_sse2_binop(nyr_x64_mach_enc_t *e,
                                           const nyr_mach_inst_t *in,
                                           int dst_off, unsigned opc) {
  int assigned = in->dst.kind == NYR_MACH_OPERAND_VREG
                     ? mach_vreg_vpreg(e, in->dst.as.reg)
                     : -1;
  unsigned dst = assigned >= 0 ? (unsigned)assigned : 14;
  int lhs_assigned = in->src0.kind == NYR_MACH_OPERAND_VREG
                         ? mach_vreg_vpreg(e, in->src0.as.reg)
                         : -1;
  int rhs_assigned = in->src1.kind == NYR_MACH_OPERAND_VREG
                         ? mach_vreg_vpreg(e, in->src1.as.reg)
                         : -1;
  unsigned lhs_scratch = 14, rhs_scratch = 15;
  if (lhs_assigned < 0 && rhs_assigned == (int)lhs_scratch)
    lhs_scratch = 15;
  if (rhs_assigned < 0 && lhs_assigned == (int)rhs_scratch)
    rhs_scratch = 14;
  unsigned lhs = lhs_scratch, rhs = rhs_scratch;
  if (!mach_load_vector_operand(e, &in->src0, lhs_scratch, &lhs) ||
      !mach_load_vector_operand(e, &in->src1, rhs_scratch, &rhs) ||
      !mach_xmm_vextractf128(e, 0, lhs, 0) ||
      !mach_xmm_vextractf128(e, 1, lhs, 1) ||
      !mach_xmm_vextractf128(e, 2, rhs, 0) ||
      !mach_xmm_vextractf128(e, 3, rhs, 1) ||
      !mach_vec_op_reg(e, 0, 2, 0x66, opc) ||
      !mach_vec_op_reg(e, 1, 3, 0x66, opc) ||
      !mach_ymm_op_reg(e, dst, dst, 0xef) ||
      !mach_xmm_vinsertf128(e, dst, dst, 0, 0) ||
      !mach_xmm_vinsertf128(e, dst, dst, 1, 1))
    return false;
  if (assigned >= 0) {
    if (e->vec_color_seeded)
      e->vec_color_seeded[in->dst.as.reg] = true;
    return true;
  }
  return mach_ymm_store_v256(e, dst_off, dst);
}

static bool mach_vector_ymm_rhs_load(nyr_x64_mach_enc_t *e, size_t load_index,
                                     size_t consumer_index,
                                     const nyr_mach_operand_t *rhs,
                                     nyr_mach_type_t vector_type);

/*
 * Binary op for V256_I64: vpaddq/vpsubq/vpand/vpor/vpxor ymm
 */
static bool mach_vector_ymm_binop(nyr_x64_mach_enc_t *e,
                                  const nyr_mach_inst_t *in, int dst_off,
                                  unsigned opc) {
  if (e->target && !(e->target->caps & NY_NATIVE_CAP_MACH_AVX2) &&
      (opc == 0xd4 || opc == 0xfb))
    return mach_vector_ymm_i64_sse2_binop(e, in, dst_off, opc);
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
  const nyr_mach_inst_t *folded_load = NULL;
  if (e->vec_fast_path && e->target &&
      (e->target->caps & NY_NATIVE_CAP_MACH_AVX2) &&
      in->opcode == NYR_MACH_ADD && e->cur_inst > 0 &&
      mach_vector_ymm_rhs_load(e, e->cur_inst - 1, e->cur_inst, &in->src1,
                               NYR_MACH_TYPE_V256_I64))
    folded_load = &e->mach->insts[e->cur_inst - 1];
  /*
   * Apply src1 with the non-destructive three-operand form: no copy of src0
   * into the destination, and a destination that shares src1's register
   * still reads src1 first.
   */
  if (folded_load && folded_load->src0.kind == NYR_MACH_OPERAND_FRAME) {
    if (!mach_ymm_op3_mem(e, d, a, mach_slot_off(e, &folded_load->src0), opc))
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
      addr_ok = bp >= 0;
      if (addr_ok) {
        base = (unsigned)bp;
        if (ip < 0 || ip == 4) {
          addr_ok = mach_load_op_rax(e, index_op);
          index = 0;
        } else {
          index = (unsigned)ip;
        }
      }
    }
    e->cur_inst = saved_inst;
    if (!addr_ok)
      return mach_err(e, "x64: lost resident ymm i64 rhs address");
    unsigned char vex2 = (unsigned char)(d >= 8 ? 0x61 : 0xe1);
    if (base >= 8)
      vex2 = (unsigned char)(vex2 & ~0x20u);
    if (index >= 8)
      vex2 = (unsigned char)(vex2 & ~0x40u);
    unsigned char vex3 = (unsigned char)(0x05u | ((~a & 0x0fu) << 3));
    ny_x64_mem_addr_t mem = {base, (int)index, scale, 0};
    if (!mach_u8(e, 0xc4) || !mach_u8(e, vex2) || !mach_u8(e, vex3) ||
        !mach_u8(e, opc) || !mach_emit_mem(e, &mem, d))
      return false;
  } else if (in->src1.kind == NYR_MACH_OPERAND_VREG) {
    int breg = mach_vreg_vpreg(e, in->src1.as.reg);
    if (breg >= 0) {
      if (!mach_seed_vector_preg(e, in->src1.as.reg) ||
          !mach_ymm_op3_reg(e, d, a, (unsigned)breg, opc))
        return false;
    } else if (!mach_ymm_op3_mem(e, d, a, mach_slot_off(e, &in->src1), opc))
      return false;
  } else if (in->src1.kind == NYR_MACH_OPERAND_FRAME) {
    if (!mach_ymm_op3_mem(e, d, a, mach_slot_off(e, &in->src1), opc))
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

/*
 * AVX2 has no VPSRAQ. Build the sign lanes with VPCMPGTQ, combine their
 * high bits with VPSRLQ, and retain exact arithmetic-shift semantics.
 */
static bool mach_ymm_qword_shift_imm(nyr_x64_mach_enc_t *e, unsigned dst,
                                     unsigned src, unsigned ext,
                                     unsigned count) {
  return mach_u8(e, 0xc4) && mach_u8(e, src >= 8 ? 0xc1 : 0xe1) &&
         mach_u8(e, 0x05 | ((~dst & 15) << 3)) && mach_u8(e, 0x73) &&
         mach_u8(e, 0xc0 | (ext << 3) | (src & 7)) && mach_u8(e, count);
}

static bool mach_vector_ymm_sar(nyr_x64_mach_enc_t *e,
                                const nyr_mach_inst_t *in, int dst_off) {
  int assigned = in->dst.kind == NYR_MACH_OPERAND_VREG
                     ? mach_vreg_vpreg(e, in->dst.as.reg)
                     : -1;
  unsigned d = assigned >= 0 ? (unsigned)assigned : 14;
  unsigned a = 14;
  if (in->src1.kind != NYR_MACH_OPERAND_IMM || in->src1.as.imm < 0 ||
      in->src1.as.imm > 63 || !mach_load_vector_operand(e, &in->src0, 14, &a))
    return mach_err(e, "x64: vec8.sar.i64 needs an immediate count 0..63");
  unsigned count = (unsigned)in->src1.as.imm;
  if (count == 0) {
    if (a != d && !mach_ymm_mov_reg(e, d, a))
      return false;
  } else if (count == 1 && a != 15) {
    /*
     * (a >>> 1) | (a & SIGN): two cycles of latency from `a` instead of
     * three. SIGN = all-ones << 63 does not depend on `a`.
     *   vpcmpeqd ymm15,ymm15,ymm15; vpsllq ymm15,ymm15,63
     *   vpand ymm15,ymm15,ymmA; vpsrlq ymmD,ymmA,1; vpor ymmD,ymmD,ymm15
     */
    if (!mach_ymm_op_reg(e, 15, 15, 0x76) ||
        !mach_ymm_qword_shift_imm(e, 15, 15, 6, 63) ||
        !mach_ymm_op_reg(e, 15, a, 0xdb) ||
        !mach_ymm_qword_shift_imm(e, d, a, 2, 1) ||
        !mach_ymm_op_reg(e, d, 15, 0xeb))
      return false;
  } else {
    /*
     * vpxor ymm15,ymm15,ymm15; vpcmpgtq ymm15,ymm15,ymmA
     */
    if (!mach_ymm_op_reg(e, 15, 15, 0xef) || !mach_u8(e, 0xc4) ||
        !mach_u8(e, a >= 8 ? 0x42 : 0x62) || !mach_u8(e, 0x05) ||
        !mach_u8(e, 0x37) || !mach_u8(e, 0xf8 | (a & 7)))
      return false;
    if (count == 63) {
      if (!mach_ymm_mov_reg(e, d, 15))
        return false;
    } else if (!mach_ymm_qword_shift_imm(e, d, a, 2, count) ||
               !mach_ymm_qword_shift_imm(e, 15, 15, 6, 64 - count) ||
               !mach_ymm_op_reg(e, d, 15, 0xeb)) {
      return false;
    }
  }
  if (assigned >= 0) {
    if (e->vec_color_seeded)
      e->vec_color_seeded[in->dst.as.reg] = true;
    return true;
  }
  return mach_ymm_store_v256(e, dst_off, d);
}

/*
 * Four-lane wrapping i64 multiply on AVX2, which has no VPMULLQ:
 *   a * b = lo(a) * lo(b) + ((hi(a) * lo(b) + lo(a) * hi(b)) << 32)
 * VPMULUDQ multiplies the low 32 bits of each lane into a full 64-bit
 * product. Only ymm14/ymm15 are scratch, so every operand is either copied
 * into one of them or read as VPMULUDQ's register/memory source, and the
 * destination is written last (it may share a source register).
 * NYTRIX_NO_VEC_MUL_I64=1 keeps the vectorizer from emitting this op.
 */
static bool mach_ymm_copy_vector_operand(nyr_x64_mach_enc_t *e,
                                         const nyr_mach_operand_t *op,
                                         unsigned scratch) {
  unsigned reg = scratch;
  if (!mach_load_vector_operand(e, op, scratch, &reg))
    return false;
  return reg == scratch || mach_ymm_mov_reg(e, scratch, reg);
}

static bool mach_ymm_pmuludq_operand(nyr_x64_mach_enc_t *e, unsigned acc,
                                     const nyr_mach_operand_t *op) {
  if (op->kind == NYR_MACH_OPERAND_VREG) {
    int preg = mach_vreg_vpreg(e, op->as.reg);
    if (preg >= 0)
      return mach_seed_vector_preg(e, op->as.reg) &&
             mach_ymm_op3_reg(e, acc, acc, (unsigned)preg, 0xf4);
    if (!mach_vreg_is_v256(e->mach, op->as.reg))
      return mach_err(e, "x64: vec8.mul.i64 operand is not V256");
  } else if (op->kind != NYR_MACH_OPERAND_FRAME) {
    return mach_err(e, "x64: unsupported vec8.mul.i64 operand");
  }
  return mach_ymm_op3_mem(e, acc, acc, mach_slot_off(e, op), 0xf4);
}

static bool mach_vector_ymm_mul_i64(nyr_x64_mach_enc_t *e,
                                    const nyr_mach_inst_t *in, int dst_off) {
  if (!e->target || !(e->target->caps & NY_NATIVE_CAP_MACH_AVX2))
    return mach_err(e, "x64: vec8.mul.i64 requires AVX2");
  int assigned = in->dst.kind == NYR_MACH_OPERAND_VREG
                     ? mach_vreg_vpreg(e, in->dst.as.reg)
                     : -1;
  const nyr_mach_operand_t *a = &in->src0, *b = &in->src1;
  /*
   * ymm15 = hi(a) * lo(b); ymm14 = hi(b) * lo(a)
   */
  if (!mach_ymm_copy_vector_operand(e, a, 15) ||
      !mach_ymm_qword_shift_imm(e, 15, 15, 2, 32) ||
      !mach_ymm_pmuludq_operand(e, 15, b) ||
      !mach_ymm_copy_vector_operand(e, b, 14) ||
      !mach_ymm_qword_shift_imm(e, 14, 14, 2, 32) ||
      !mach_ymm_pmuludq_operand(e, 14, a))
    return false;
  /*
   * ymm14 = (cross terms) << 32; ymm15 = lo(a) * lo(b)
   */
  if (!mach_ymm_op_reg(e, 14, 15, 0xd4) ||
      !mach_ymm_qword_shift_imm(e, 14, 14, 6, 32) ||
      !mach_ymm_copy_vector_operand(e, a, 15) ||
      !mach_ymm_pmuludq_operand(e, 15, b))
    return false;
  if (assigned >= 0) {
    if (!mach_ymm_op3_reg(e, (unsigned)assigned, 15, 14, 0xd4))
      return false;
    if (e->vec_color_seeded)
      e->vec_color_seeded[in->dst.as.reg] = true;
    return true;
  }
  return mach_ymm_op_reg(e, 15, 14, 0xd4) &&
         mach_ymm_store_v256(e, dst_off, 15);
}

/*
 * vpsrlq ymmD, ymmA, imm: packed logical i64 shift right.
 */
static bool mach_vector_ymm_shr(nyr_x64_mach_enc_t *e,
                                const nyr_mach_inst_t *in, int dst_off) {
  int assigned = in->dst.kind == NYR_MACH_OPERAND_VREG
                     ? mach_vreg_vpreg(e, in->dst.as.reg)
                     : -1;
  unsigned d = assigned >= 0 ? (unsigned)assigned : 14;
  unsigned a = 14;
  if (in->src1.kind != NYR_MACH_OPERAND_IMM || in->src1.as.imm < 0 ||
      in->src1.as.imm > 63 || !mach_load_vector_operand(e, &in->src0, 14, &a))
    return mach_err(e, "x64: vec8.shr.i64 needs an immediate count 0..63");
  unsigned count = (unsigned)in->src1.as.imm;
  if (count == 0) {
    if (a != d && !mach_ymm_mov_reg(e, d, a))
      return false;
  } else if (!mach_ymm_qword_shift_imm(e, d, a, 2, count)) {
    return false;
  }
  if (assigned >= 0) {
    if (e->vec_color_seeded)
      e->vec_color_seeded[in->dst.as.reg] = true;
    return true;
  }
  return mach_ymm_store_v256(e, dst_off, d);
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
 * Describe a directly encodable YMM load that its next arithmetic consumer
 * can use as a memory operand.
 */
static bool mach_vector_ymm_rhs_load(nyr_x64_mach_enc_t *e, size_t load_index,
                                     size_t consumer_index,
                                     const nyr_mach_operand_t *rhs,
                                     nyr_mach_type_t vector_type) {
  if (!e || !e->mach || !rhs || load_index >= e->mach->inst_len ||
      consumer_index >= e->mach->inst_len || consumer_index != load_index + 1 ||
      mach_block_for_inst(e, load_index) !=
          mach_block_for_inst(e, consumer_index))
    return false;
  const nyr_mach_inst_t *load = &e->mach->insts[load_index];
  const nyr_mach_inst_t *consumer = &e->mach->insts[consumer_index];
  if (load->opcode != NYR_MACH_LOAD ||
      load->dst.kind != NYR_MACH_OPERAND_VREG ||
      mach_v128_type(e->mach, &load->dst) != vector_type ||
      mach_v128_type(e->mach, &consumer->dst) != vector_type ||
      (vector_type == NYR_MACH_TYPE_V256_F64 &&
       consumer->opcode != NYR_MACH_ADD && consumer->opcode != NYR_MACH_SUB &&
       consumer->opcode != NYR_MACH_MUL && consumer->opcode != NYR_MACH_DIV) ||
      (vector_type == NYR_MACH_TYPE_V256_I64 &&
       consumer->opcode != NYR_MACH_ADD) ||
      !mach_op_is_vreg(rhs, load->dst.as.reg) ||
      mach_inst_input_uses_vreg(consumer, load->dst.as.reg) != 1 ||
      mach_op_is_vreg(&consumer->src0, load->dst.as.reg) ||
      mach_op_is_vreg(&consumer->src2, load->dst.as.reg))
    return false;
  for (size_t i = consumer_index + 1; i < e->mach->inst_len; ++i)
    if (mach_inst_uses_vreg(&e->mach->insts[i], load->dst.as.reg))
      return false;

  /*
   * Initially restrict the fold to addresses the current YMM load path can
   * encode without materializing a scratch address.
   */
  if (load->src0.kind == NYR_MACH_OPERAND_FRAME)
    return true;
  if (load->src0.kind != NYR_MACH_OPERAND_VREG)
    return false;
  const nyr_mach_operand_t *base = NULL, *index = NULL;
  unsigned scale = 0;
  size_t saved_inst = e->cur_inst;
  e->cur_inst = load_index;
  if (!mach_sib_lea_mem(e, load->src0.as.reg, load_index, &base, &index, &scale,
                        NULL)) {
    e->cur_inst = saved_inst;
    return false;
  }
  bool invalid = base->kind != NYR_MACH_OPERAND_VREG ||
                 index->kind != NYR_MACH_OPERAND_VREG ||
                 mach_vreg_preg(e, base->as.reg) < 0 ||
                 mach_vreg_preg(e, index->as.reg) == 4 ||
                 (scale != 1 && scale != 2 && scale != 4 && scale != 8);
  e->cur_inst = saved_inst;
  return !invalid;
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
  /*
   * AVX is three-operand here: VEX.vvvv carries src0, so don't copy it to dst.
   */
  unsigned char vex3 = (unsigned char)((((~a_reg) & 0xfu) << 3) | 0x05u);
  const nyr_mach_inst_t *folded_load = NULL;
  if (e->cur_inst > 0 &&
      mach_vector_ymm_rhs_load(e, e->cur_inst - 1, e->cur_inst, &in->src1,
                               NYR_MACH_TYPE_V256_F64))
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
      addr_ok = bp >= 0;
      if (addr_ok) {
        base = (unsigned)bp;
        if (ip < 0 || ip == 4) {
          /*
           * The folded YMM load and its consumer are adjacent, so RAX can
           * reload a spilled index and serve directly as the SIB index.
           */
          addr_ok = mach_load_op_rax(e, index_op);
          index = 0;
        } else {
          index = (unsigned)ip;
        }
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
    const nyr_mach_live_segment_t *src1_seg =
        mach_vsegment_at(e, in->src1.as.reg, e->cur_inst);
    /*
     * AVX accepts a memory rhs.  A spilled, unseeded vreg already has its
     * canonical value in its home, so avoid loading it into a vector register
     * only to use it once.  Keeping it unseeded also avoids a redundant
     * live-out save of that unchanged value.
     */
    if (breg >= 0 && src1_seg && src1_seg->spill && e->vec_color_seeded &&
        !e->vec_color_seeded[in->src1.as.reg])
      breg = -1;
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

static bool mach_inst_uses_v256_i64(const nyr_mach_func_t *mach,
                                    const nyr_mach_inst_t *in) {
  const nyr_mach_operand_t *ops[] = {&in->dst, &in->src0, &in->src1, &in->src2};
  for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); ++i)
    if (mach_v128_type(mach, ops[i]) == NYR_MACH_TYPE_V256_I64)
      return true;
  for (size_t i = 0; i < in->args_len; ++i)
    if (mach_v128_type(mach, &in->args[i]) == NYR_MACH_TYPE_V256_I64)
      return true;
  return false;
}

static bool mach_inst_uses_v256(const nyr_mach_func_t *mach,
                                const nyr_mach_inst_t *in) {
  const nyr_mach_operand_t *ops[] = {&in->dst, &in->src0, &in->src1, &in->src2};
  for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); ++i)
    if (mach_v128_type(mach, ops[i]) == NYR_MACH_TYPE_V256_I64 ||
        mach_v128_type(mach, ops[i]) == NYR_MACH_TYPE_V256_F64)
      return true;
  for (size_t i = 0; i < in->args_len; ++i) {
    nyr_mach_type_t type = mach_v128_type(mach, &in->args[i]);
    if (type == NYR_MACH_TYPE_V256_I64 || type == NYR_MACH_TYPE_V256_F64)
      return true;
  }
  return false;
}

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

/*
 * True when the function has a scalar F32/F64 value, which may be resident
 * in (or use as scratch) xmm0..xmm7.
 */
static bool mach_scalar_float_types_present(const nyr_mach_func_t *mach) {
  if (!mach || !mach->vreg_types)
    return true;
  for (size_t i = 0; i < mach->vreg_len; ++i)
    if (mach->vreg_types[i] == NYR_MACH_TYPE_F32 ||
        mach->vreg_types[i] == NYR_MACH_TYPE_F64)
      return true;
  for (size_t i = 0; i < mach->frame_slot_len; ++i)
    if (mach->frame_slots[i].type == NYR_MACH_TYPE_F32 ||
        mach->frame_slots[i].type == NYR_MACH_TYPE_F64)
      return true;
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
  /*
   * Code emitted here runs only when the block falls through.
   */
  nyr_mach_opcode_t last_op = e->mach->insts[last].opcode;
  bool falls_through = last_op != NYR_MACH_BR && last_op != NYR_MACH_BR_IF &&
                       last_op != NYR_MACH_RET;
  for (uint32_t v = 0; v < e->vec_colors_len; ++v) {
    if (!nyr_mach_regalloc_live_out(&e->vec_regalloc, block, v) ||
        !e->vec_color_seeded || !e->vec_color_seeded[v])
      continue;
    const nyr_mach_live_segment_t *seg = mach_vsegment_at(e, v, last);
    if (!seg || seg->color < 0)
      continue;
    /*
     * Publish a single-definition value once, at the end of its defining
     * block: that block dominates every use, so later flushes (typically one
     * per loop latch for an invariant broadcast) can skip their store.
     */
    if (falls_through && mach_vector_publish_once_in_def_block(e, v, seg) < 0)
      return false;
    if (e->vector_loop_carry_seeding && seg->carried && !seg->spill)
      continue;
    if (!mach_vector_publish_home(e, v, seg))
      return false;
    e->vec_color_seeded[v] = false;
  }
  return true;
}

/*
 * Publish resident scalar floats to their homes. At a call (`at_branch`
 * false) every XMM register is clobbered, so dedicated loop-FPR values lose
 * residency too. At a branch nothing is clobbered: a loop-FPR value keeps
 * its dedicated register on the fall-through path. Unseeding it there made
 * the next use reload a home that loop-FPR definitions never write (an f64
 * constant hoisted above a bounds check read garbage at -O0/-O1).
 */
static bool mach_flush_float_to_home_mode(nyr_x64_mach_enc_t *e,
                                          bool at_branch) {
  if (!e || !e->fp_fast_path || !e->fp_color_seeded)
    return true;
  for (uint32_t v = mach_next_set(e->fp_color_seeded, 0, e->fp_colors_len);
       v < e->fp_colors_len;
       v = mach_next_set(e->fp_color_seeded, v + 1, e->fp_colors_len)) {
    if (e->loop_fpr && e->loop_fpr[v] >= 0) {
      if (!at_branch)
        e->fp_color_seeded[v] = false;
      continue;
    }
    const nyr_mach_live_segment_t *seg = mach_fsegment_at(e, v, e->cur_inst);
    if (!seg || seg->color < 0)
      continue;
    /*
     * A value that was only reloaded from its home in this block, and not
     * redefined since, is already published. Storing it again would put a
     * store-to-load forward on every iteration of a loop that reads it.
     */
    if (e->fp_seed_inst && e->cur_block_valid &&
        e->fp_seed_inst[v] > e->mach->blocks[e->cur_block].first_inst &&
        e->fp_last_def_inst[v] < e->fp_seed_inst[v]) {
      e->fp_color_seeded[v] = false;
      continue;
    }
    if (!mach_store_xmm(e, mach_vreg_home_off(e, v),
                        e->mach->vreg_types[v] == NYR_MACH_TYPE_F32,
                        (unsigned)seg->color))
      return false;
    e->fp_color_seeded[v] = false;
  }
  return true;
}

static bool mach_flush_vector_to_home(nyr_x64_mach_enc_t *e,
                                      bool preserve_loop_carry) {
  if (!e || !e->vec_fast_path || !e->vec_color_seeded)
    return true;
  for (uint32_t v = mach_next_set(e->vec_color_seeded, 0, e->vec_colors_len);
       v < e->vec_colors_len;
       v = mach_next_set(e->vec_color_seeded, v + 1, e->vec_colors_len)) {
    const nyr_mach_live_segment_t *seg = mach_vsegment_at(e, v, e->cur_inst);
    if (!seg || seg->color < 0)
      continue;
    /*
     * The allocator's carry proof guarantees a common physical color through
     * every carried successor. Exit segments retain spill=true, so they are
     * still published here for consumers outside the carried region. Calls
     * never use this exception because caller-saved vector registers do not
     * survive the ABI boundary.
     */
    if (preserve_loop_carry &&
        mach_vector_publish_once_in_def_block(e, v, seg) < 0)
      return false;
    if (preserve_loop_carry && e->vector_loop_carry_seeding && seg->carried &&
        !seg->spill)
      continue;
    if (!mach_vector_publish_home(e, v, seg))
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
  if (e->fp_seed_inst && vreg < e->fp_colors_len)
    e->fp_seed_inst[vreg] = e->cur_inst + 1;
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
  int breg = in->src1.kind == NYR_MACH_OPERAND_VREG
                 ? mach_vreg_fpreg(e, in->src1.as.reg)
                 : -1;
  if (e->vex) {
    /*
     * VEX.vvvv carries the independent LHS. Loading it into the result
     * first only adds vmovaps and can overwrite a dying RHS in that color.
     * XMM14 is reserved, so a spilled LHS cannot clobber a colored RHS.
     */
    if (!mach_load_float_operand(e, &in->src0, f32, 14, &a))
      return false;
    if (breg >= 0) {
      if (!mach_seed_float_preg(e, in->src1.as.reg) ||
          !mach_vex(e, f32 ? 2 : 3, false, d, 0, (unsigned)breg, a, opc) ||
          !mach_u8(e, 0xc0 | ((d & 7) << 3) | ((unsigned)breg & 7)))
        return false;
    } else if (in->src1.kind == NYR_MACH_OPERAND_VREG ||
               in->src1.kind == NYR_MACH_OPERAND_FRAME) {
      if (!mach_vex(e, f32 ? 2 : 3, false, d, 0, 5, a, opc) ||
          !mach_u8(e, 0x85 | ((d & 7) << 3)) ||
          !mach_i32(e, mach_slot_off(e, &in->src1)))
        return false;
    } else {
      return mach_err(e, "x64 machine form encode: unsupported float rhs");
    }
    if (assigned >= 0) {
      if (e->fp_color_seeded)
        e->fp_color_seeded[in->dst.as.reg] = true;
      return true;
    }
    return mach_store_xmm(e, dst_off, f32, d);
  }
  bool rhs_saved = false;
  /*
   * Scalar add/multiply are commutative: consume the dying RHS in place
   * instead of copying it out and then copying the LHS over it. XMM14 is
   * reserved scratch, so materializing an uncolored LHS cannot clobber d.
   */
  if (assigned >= 0 && breg == assigned && (opc == 0x58 || opc == 0x59)) {
    if (!mach_seed_float_preg(e, in->src1.as.reg) ||
        !mach_load_float_operand(e, &in->src0, f32, 14, &a) ||
        !mach_sse_op_reg(e, d, a, f32, opc))
      return false;
    if (e->fp_color_seeded)
      e->fp_color_seeded[in->dst.as.reg] = true;
    return true;
  }
  /*
   * A dying RHS may share the result color on x64 for commutative scalar
   * add/multiply. Preserve that RHS before loading src0 into the destructive
   * destination; otherwise the two-address sequence computes src0 op src0.
   * XMM15 is outside the allocator's scalar color range and is scratch for
   * this one instruction.
   */
  if (breg >= 0 && (unsigned)breg == d) {
    int areg = in->src0.kind == NYR_MACH_OPERAND_VREG
                   ? mach_vreg_fpreg(e, in->src0.as.reg)
                   : -1;
    if (areg != (int)d) {
      if (!mach_seed_float_preg(e, in->src1.as.reg) ||
          !mach_sse_mov_reg(e, 15, (unsigned)breg, f32))
        return false;
      breg = 15;
      rhs_saved = true;
    }
  }
  if (!mach_load_float_operand(e, &in->src0, f32, d, &a))
    return false;
  if (a != d && !mach_sse_mov_reg(e, d, a, f32))
    return false;
  if (in->src1.kind == NYR_MACH_OPERAND_VREG) {
    if (breg >= 0) {
      if ((!rhs_saved && !mach_seed_float_preg(e, in->src1.as.reg)) ||
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
 * A dedicated scalar-float home normally keeps its stack slot current after
 * every definition so a reload after a residency kill sees the latest value.
 * The store is dead when the value never leaves its block and every later
 * use reads the register: all remaining uses are float arithmetic or float
 * copies in the same block, with no residency kill (calls, conversions,
 * square roots, intrinsics, inline assembly, FMA, shuffles) or redefinition
 * of the same color before the last use. Anything else keeps the store.
 */
static bool mach_loop_fpr_home_needed(const nyr_x64_mach_enc_t *e, uint32_t v,
                                      size_t block, size_t pc) {
  const nyr_mach_func_t *m = e->mach;
  if (block >= m->block_len || v >= m->vreg_len ||
      nyr_mach_regalloc_live_out(&e->fp_regalloc, block, v))
    return true;
  const nyr_mach_block_t *b = &m->blocks[block];
  size_t end = (size_t)b->first_inst + b->inst_count;
  size_t last_use = SIZE_MAX;
  for (size_t i = pc + 1; i < end && i < m->inst_len; ++i)
    if (mach_inst_uses_vreg(&m->insts[i], v))
      last_use = i;
  if (last_use == SIZE_MAX)
    return false; /* Dead after its definition. */
  int color = e->loop_fpr[v];
  for (size_t i = pc + 1; i <= last_use; ++i) {
    const nyr_mach_inst_t *in = &m->insts[i];
    /*
     * A CONVERT only clobbers the loop colors its encoding writes; the
     * same-color definition and use checks below handle the rest.
     */
    if ((in->opcode == NYR_MACH_CONVERT || in->opcode == NYR_MACH_SQRT) &&
        ((mach_convert_loop_fpr_clobbers(m, in, e->loop_fpr) >> color) & 1u))
      return true;
    switch (in->opcode) {
    case NYR_MACH_CALL:
    case NYR_MACH_INTRINSIC:
    case NYR_MACH_INLINE_ASM:
    case NYR_MACH_FMA:
    case NYR_MACH_SHUFFLE:
      return true;
    default:
      break;
    }
    if (i < last_use && in->dst.kind == NYR_MACH_OPERAND_VREG &&
        in->opcode != NYR_MACH_STORE && in->dst.as.reg != v &&
        in->dst.as.reg < e->fp_colors_len &&
        e->loop_fpr[in->dst.as.reg] == color)
      return true;
    if (!mach_inst_uses_vreg(in, v))
      continue;
    bool float_arith =
        (in->opcode == NYR_MACH_ADD || in->opcode == NYR_MACH_SUB ||
         in->opcode == NYR_MACH_MUL || in->opcode == NYR_MACH_DIV) &&
        mach_is_float(m, &in->dst);
    bool float_copy =
        in->opcode == NYR_MACH_COPY && in->dst.kind == NYR_MACH_OPERAND_VREG &&
        mach_is_float(m, &in->dst) && in->src0.kind == NYR_MACH_OPERAND_VREG &&
        in->src0.as.reg == v;
    if ((!float_arith && !float_copy) || in->args_len)
      return true;
  }
  return false;
}

/*
 * Reserve independent scalar colors and prove which ones survive each CFG
 * edge. Calls kill residency; definitions restore it. Homes remain current
 * so a path following a call can reload without depending on layout order.
 *
 * Dedicated loop FPR colors that a CONVERT or SQRT overwrites besides its
 * own destination. Mirrors the CONVERT encoder: int->float writes the
 * destination's register when it has a loop home and xmm0 otherwise;
 * float<->float works in the destination color or xmm14 scratch; float->int
 * stages the source in xmm0. Ordinary float colors below the loop-color count
 * are respilled once loop homes exist, so they never alias a loop color.
 */
static unsigned mach_convert_loop_fpr_clobbers(const nyr_mach_func_t *m,
                                               const nyr_mach_inst_t *in,
                                               const int *loop_regs) {
  if (in && in->opcode == NYR_MACH_SQRT)
    return 0u; /* sqrtsd/ss works in the destination color or xmm14. */
  if (!in || in->opcode != NYR_MACH_CONVERT)
    return 0;
  bool dst_float = mach_is_f64(m, &in->dst) || mach_is_f32(m, &in->dst);
  bool src_float = mach_is_f64(m, &in->src0) || mach_is_f32(m, &in->src0);
  if (dst_float && mach_is_i64(m, &in->src0))
    return in->dst.kind == NYR_MACH_OPERAND_VREG && loop_regs &&
                   in->dst.as.reg < m->vreg_len &&
                   loop_regs[in->dst.as.reg] >= 0
               ? 0u
               : 1u;
  if (dst_float && src_float)
    return 0u;
  if (mach_is_i64(m, &in->dst) && src_float)
    return 1u;
  return ~0u;
}

static void mach_allocate_loop_fprs(nyr_x64_mach_enc_t *e, const char *name) {
  const nyr_mach_func_t *m = e->mach;
  size_t nv = m->vreg_len, nb = m->block_len;
  if (!e->fp_fast_path || e->vec_fast_path || !nv || nv > 512 ||
      nb > MACH_SET_BLOCKS || !nb || getenv("NYTRIX_FORCE_STACK_FPR"))
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
      /*
       * Conversions may use low XMM scratch registers. Model the clobber in
       * loop-home dataflow instead of disabling loop homes for the function.
       */
      if (in->opcode == NYR_MACH_SHUFFLE || in->opcode == NYR_MACH_FMA)
        goto done;
      if (in->src1.kind == NYR_MACH_OPERAND_BLOCK) {
        size_t target = in->src1.as.block_index;
        edges[b * nb + target] = true;
        loop |= target <= b;
      }
      if (in->dst.kind == NYR_MACH_OPERAND_VREG &&
          in->opcode != NYR_MACH_STORE) {
        size_t v = in->dst.as.reg;
        /*
         * SQRT and int->float CONVERT encode straight into an assigned
         * destination color (see their encoders), so they may define a
         * dedicated home too.
         */
        bool defines_in_place =
            in->opcode == NYR_MACH_SQRT ||
            (in->opcode == NYR_MACH_CONVERT && mach_is_i64(m, &in->src0));
        if (in->opcode != NYR_MACH_COPY && in->opcode != NYR_MACH_ADD &&
            in->opcode != NYR_MACH_SUB && in->opcode != NYR_MACH_MUL &&
            in->opcode != NYR_MACH_DIV && !defines_in_place)
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
  /*
   * Destructive scalar operations may reuse a dying left operand. Only
   * coalesce complete classes whose allocator intervals never overlap,
   * except at that instruction's read-before-write endpoint.
   */
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
        state &= (uint8_t)~mach_convert_loop_fpr_clobbers(m, in, regs);
        if (in->dst.kind == NYR_MACH_OPERAND_VREG &&
            in->opcode != NYR_MACH_STORE && regs[in->dst.as.reg] >= 0)
          state |= (uint8_t)(1u << regs[in->dst.as.reg]);
      }
      changed |= state != out[b];
      out[b] = state;
    }
  } while (changed);
  e->loop_fpr = regs;
  e->loop_fpr_entry = entry;
  regs = NULL;
  entry = NULL;
  /*
   * Ordinary segments that the linear scan placed in a color now reserved
   * for a loop home move to a free color above the loop colors when one is
   * clobber-free across their whole interval and unused by any overlapping
   * segment; otherwise they spill as before.
   */
  size_t seg_len = e->fp_regalloc.segment_len;
  nyr_mach_live_segment_t *segs = e->fp_regalloc.segments;
  for (size_t i = 0; i < seg_len; ++i) {
    nyr_mach_live_segment_t *s = &segs[i];
    int c = e->loop_fpr[s->vreg];
    if (c < 0 && !(s->color >= 0 && (unsigned)s->color < count))
      continue;
    int moved = -1;
    if (c < 0 && !s->carried && !s->acyclic_carried) {
      for (unsigned cand = count; cand < e->fp_color_limit && moved < 0;
           ++cand) {
        bool free_color = true;
        for (size_t j = 0; j < seg_len && free_color; ++j) {
          const nyr_mach_live_segment_t *o = &segs[j];
          if (j != i && o->color == (int)cand && o->start <= s->end &&
              s->start <= o->end)
            free_color = false;
        }
        for (size_t pc = s->start;
             free_color && pc <= s->end && pc < m->inst_len; ++pc)
          if (nyr_mach_inst_fixed_clobbers(m, &m->insts[pc],
                                           NYR_MACH_REGCLASS_FPR) &
              (UINT64_C(1) << cand))
            free_color = false;
        if (free_color)
          moved = (int)cand;
      }
    }
    s->color = c >= 0 ? c : moved;
    s->carried = s->acyclic_carried = false;
    /*
     * A segment moved to a free color keeps the allocator's reload/spill
     * flags: neighbouring segments of the same vreg compare colors before
     * carrying (mach_fsegment_carries), so a color change already routes
     * them through the home. Forcing spill here stored every block-local
     * temporary to its dead home once per loop iteration.
     */
    if (c >= 0 || moved < 0) {
      s->reload = s->spill = true;
    } else {
      /*
       * A moved segment no longer shares a color with its vreg's other
       * segments, so any neighbour goes through the home: publish it when a
       * later segment exists (it reloads, possibly after a gap the
       * end-of-segment check cannot see) and reload when an earlier one
       * does. A block-local temporary has neither and keeps no dead home.
       */
      for (size_t j = 0; j < seg_len; ++j) {
        const nyr_mach_live_segment_t *o = &segs[j];
        if (j == i || o->vreg != s->vreg)
          continue;
        if (o->start > s->end)
          s->spill = true;
        if (o->end < s->start)
          s->reload = true;
      }
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
        (in->opcode != NYR_MACH_CALL || getenv("NYTRIX_NO_FP_CALLS")) &&
        in->opcode != NYR_MACH_SUB && in->opcode != NYR_MACH_MUL &&
        in->opcode != NYR_MACH_DIV && in->opcode != NYR_MACH_FMA &&
        in->opcode != NYR_MACH_SQRT && in->opcode != NYR_MACH_SIN &&
        in->opcode != NYR_MACH_COS && in->opcode != NYR_MACH_CONVERT &&
        in->opcode != NYR_MACH_SHUFFLE && in->opcode != NYR_MACH_LOAD &&
        in->opcode != NYR_MACH_STORE && in->opcode != NYR_MACH_CMP &&
        in->opcode != NYR_MACH_BR && in->opcode != NYR_MACH_BR_IF &&
        in->opcode != NYR_MACH_NOP && in->opcode != NYR_MACH_RET &&
        in->opcode != NYR_MACH_REDUCE_ADD)
      return false;
  }
  return has_float;
}

/*
 * RSP-based ModRM+SIB: always a SIB byte, and disp8 whenever the offset fits
 * (mod=01) so stack staging stays three bytes shorter than disp32.
 */
static bool mach_rsp_disp(nyr_x64_mach_enc_t *e, unsigned reg, int off) {
  unsigned r = (reg & 7) << 3;
  bool narrow = !mach_no_frame_disp8();
  if (narrow && !off)
    return mach_u8(e, (unsigned char)(0x04 | r)) && mach_u8(e, 0x24);
  if (narrow && off >= -128 && off <= 127)
    return mach_u8(e, (unsigned char)(0x44 | r)) && mach_u8(e, 0x24) &&
           mach_u8(e, (unsigned char)off);
  return mach_u8(e, (unsigned char)(0x84 | r)) && mach_u8(e, 0x24) &&
         mach_i32(e, off);
}

static bool mach_load_eax(nyr_x64_mach_enc_t *e, int off) {
  mach_rax_invalidate(e);
  return mach_u8(e, 0x8b) && mach_rbp_disp(e, 0, off);
}

static bool mach_store_eax_rsp(nyr_x64_mach_enc_t *e, int off) {
  return mach_u8(e, 0x89) && mach_rsp_disp(e, 0, off);
}

static bool mach_store_rax_rsp(nyr_x64_mach_enc_t *e, int off) {
  return mach_rex(e, true, false, false, false) && mach_u8(e, 0x89) &&
         mach_rsp_disp(e, 0, off);
}

static bool mach_load_rax_rsp(nyr_x64_mach_enc_t *e, int off) {
  mach_rax_invalidate(e);
  return mach_rex(e, true, false, false, false) && mach_u8(e, 0x8b) &&
         mach_rsp_disp(e, 0, off);
}

static bool mach_store_gpr(nyr_x64_mach_enc_t *e, int off, int reg) {
  return mach_rex(e, true, reg >= 8, false, false) && mach_u8(e, 0x89) &&
         mach_rbp_disp(e, (unsigned)reg, off);
}

static bool mach_prepare_direct_params(nyr_x64_mach_enc_t *e) {
  const nyr_mach_func_t *mach = e->mach;
  bool trace = ny_dbg_is_enabled(NY_DBG_DEEP);
  /*
   * The home-slot table exists whenever parameter ABI data does.
   */
  bool has_aggregate_param = false;
  for (size_t p = 0; mach && mach->param_aggregate_home_slots &&
                     mach->param_types && p < mach->param_count;
       ++p)
    has_aggregate_param |= mach->param_types[p] == NYIR_PARAM_AGGREGATE;
  if (!mach || !mach->param_count || mach->param_count > mach->frame_slot_len ||
      has_aggregate_param || !mach->block_len || mach->param_count > 6) {
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

  /*
   * Direct reads take parameters from the incoming argument registers, which
   * only hold them on the first entry. A branch back to the entry block (a
   * self tail call rewritten as a loop) re-executes those loads after
   * storing the new arguments to the parameter slots, so they must read the
   * slots.
   */
  for (size_t i = 0; i < mach->inst_len; ++i) {
    const nyr_mach_inst_t *in = &mach->insts[i];
    if ((in->opcode == NYR_MACH_BR || in->opcode == NYR_MACH_BR_IF) &&
        in->src1.kind == NYR_MACH_OPERAND_BLOCK && in->src1.as.block_index == 0)
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

/*
 * Shrink-wrapped base case. A recursive function whose entry block only
 * compares its parameters (or a parameter with a constant) and branches to a
 * block that returns a constant or a parameter can take that exit before the
 * prologue: the base case of towers/fib/ackermann-style recursion is about
 * half of all calls and otherwise pays every callee-save push, the frame and
 * the parameter spills. The normal entry still evaluates the same test.
 * NYTRIX_NO_EARLY_EXIT=1 disables it.
 */
static int mach_early_operand(const nyr_mach_func_t *m, const int *param_reg,
                              const int64_t *imm, const bool *imm_ok,
                              const nyr_mach_operand_t *op, int64_t *out_imm) {
  /*
   * >= 0: argument register; -1: immediate in *out_imm; -2: unsupported
   */
  if (op->kind == NYR_MACH_OPERAND_IMM) {
    *out_imm = op->as.imm;
    return -1;
  }
  if (op->kind != NYR_MACH_OPERAND_VREG || op->as.reg >= m->vreg_len)
    return -2;
  if (param_reg[op->as.reg] >= 0)
    return param_reg[op->as.reg];
  if (imm_ok[op->as.reg]) {
    *out_imm = imm[op->as.reg];
    return -1;
  }
  return -2;
}

static nyr_mach_cond_t mach_cond_swap(nyr_mach_cond_t c) {
  switch (c) {
  case NYR_MACH_COND_LT:
    return NYR_MACH_COND_GT;
  case NYR_MACH_COND_GT:
    return NYR_MACH_COND_LT;
  case NYR_MACH_COND_LE:
    return NYR_MACH_COND_GE;
  case NYR_MACH_COND_GE:
    return NYR_MACH_COND_LE;
  case NYR_MACH_COND_B:
    return NYR_MACH_COND_A;
  case NYR_MACH_COND_A:
    return NYR_MACH_COND_B;
  case NYR_MACH_COND_BE:
    return NYR_MACH_COND_AE;
  case NYR_MACH_COND_AE:
    return NYR_MACH_COND_BE;
  default:
    return c;
  }
}

static bool mach_emit_return(nyr_x64_mach_enc_t *e) {
  if (e->mach->x64_return_thunk)
    return mach_u8(e, 0xe9) &&
           mach_add_reloc(e, "__x86_return_thunk", NY_RELOC_PLT32) &&
           mach_i32(e, 0);
  return mach_u8(e, 0xc3);
}

static bool mach_emit_early_exit(nyr_x64_mach_enc_t *e, const char *name) {
  const nyr_mach_func_t *m = e->mach;
  if (!m || !m->block_len || !m->param_count || m->param_count > 6 ||
      m->param_count > m->frame_slot_len || m->vreg_len > 4096 || !e->target ||
      e->target->abi != NY_NATIVE_ABI_SYSV || getenv("NYTRIX_NO_EARLY_EXIT"))
    return true;
  if (name && (strstr(name, "rt_main") || !strcmp(name, "main")))
    return true;
  for (size_t p = 0; p < m->param_count; ++p) {
    nyr_mach_type_t t = m->frame_slots[p].type;
    if (t == NYR_MACH_TYPE_F32 || t == NYR_MACH_TYPE_F64 ||
        m->frame_slots[p].size > 8 ||
        (m->param_types && m->param_types[p] == NYIR_PARAM_AGGREGATE))
      return true;
  }
  static const int gp_sysv[] = {7, 6, 2, 1, 8, 9};
  int *param_reg = malloc(m->vreg_len * sizeof(*param_reg));
  int64_t *imm = calloc(m->vreg_len, sizeof(*imm));
  bool *imm_ok = calloc(m->vreg_len, sizeof(*imm_ok));
  bool ok = param_reg && imm && imm_ok;
  if (!ok) {
    free(param_reg);
    free(imm);
    free(imm_ok);
    return true;
  }
  for (size_t v = 0; v < m->vreg_len; ++v)
    param_reg[v] = -1;
  const nyr_mach_block_t *entry = &m->blocks[0];
  const nyr_mach_inst_t *cmp = NULL, *br = NULL;
  bool shape = entry->first_inst <= m->inst_len &&
               entry->inst_count <= m->inst_len - entry->first_inst;
  for (size_t i = entry->first_inst;
       shape && i < entry->first_inst + entry->inst_count; ++i) {
    const nyr_mach_inst_t *in = &m->insts[i];
    if (in->opcode == NYR_MACH_LOAD &&
        in->src0.kind == NYR_MACH_OPERAND_FRAME &&
        in->src0.as.frame_index < m->param_count &&
        in->dst.kind == NYR_MACH_OPERAND_VREG && in->dst.as.reg < m->vreg_len &&
        !cmp)
      param_reg[in->dst.as.reg] = gp_sysv[in->src0.as.frame_index];
    else if (in->opcode == NYR_MACH_COPY &&
             in->src0.kind == NYR_MACH_OPERAND_IMM &&
             in->dst.kind == NYR_MACH_OPERAND_VREG &&
             in->dst.as.reg < m->vreg_len && !mach_is_float(m, &in->dst)) {
      imm[in->dst.as.reg] = in->src0.as.imm;
      imm_ok[in->dst.as.reg] = true;
    } else if (in->opcode == NYR_MACH_CMP && !cmp &&
               in->dst.kind == NYR_MACH_OPERAND_VREG &&
               mach_cmov_cc(in->condition) >= 0 && !mach_is_float(m, &in->src0))
      cmp = in;
    else if (in->opcode == NYR_MACH_BR_IF && cmp && !br &&
             i + 1 == entry->first_inst + entry->inst_count &&
             in->src0.kind == NYR_MACH_OPERAND_VREG &&
             in->src0.as.reg == cmp->dst.as.reg &&
             in->src1.kind == NYR_MACH_OPERAND_BLOCK)
      br = in;
    else
      shape = false;
  }
  int64_t ia = 0, ib = 0, rimm = 0;
  int ra = -2, rb = -2, rret = -2;
  if (shape && br) {
    ra = mach_early_operand(m, param_reg, imm, imm_ok, &cmp->src0, &ia);
    rb = mach_early_operand(m, param_reg, imm, imm_ok, &cmp->src1, &ib);
    uint32_t t = br->src1.as.block_index;
    shape = ra != -2 && rb != -2 && (ra >= 0 || rb >= 0) && t < m->block_len;
    if (shape) {
      /*
       * Taken block: constant copies, then RET of a constant or parameter.
       */
      const nyr_mach_block_t *tb = &m->blocks[t];
      for (size_t i = tb->first_inst;
           shape && i < tb->first_inst + tb->inst_count; ++i) {
        const nyr_mach_inst_t *in = &m->insts[i];
        if (in->opcode == NYR_MACH_COPY &&
            in->src0.kind == NYR_MACH_OPERAND_IMM &&
            in->dst.kind == NYR_MACH_OPERAND_VREG &&
            in->dst.as.reg < m->vreg_len && !mach_is_float(m, &in->dst)) {
          imm[in->dst.as.reg] = in->src0.as.imm;
          imm_ok[in->dst.as.reg] = true;
        } else if (in->opcode == NYR_MACH_RET &&
                   i + 1 == tb->first_inst + tb->inst_count &&
                   !mach_is_float(m, &in->src0)) {
          rret =
              mach_early_operand(m, param_reg, imm, imm_ok, &in->src0, &rimm);
          shape = rret != -2;
        } else
          shape = false;
      }
      shape = shape && rret != -2;
    }
  }
  free(param_reg);
  free(imm);
  free(imm_ok);
  if (!shape || !br || rret == -2)
    return true;
  nyr_mach_cond_t cond = cmp->condition;
  if (ra < 0) { /* immediate on the left: swap so the register is compared */
    int t = ra;
    ra = rb;
    rb = t;
    int64_t ti = ia;
    ia = ib;
    ib = ti;
    cond = mach_cond_swap(cond);
  }
  int cc = mach_cmov_cc(cond);
  if (cc < 0)
    return true;
  if (rb < 0) {
    if (ib < INT32_MIN || ib > INT32_MAX)
      return true;
    bool small = ib >= -128 && ib <= 127;
    if (ib == 0 && (cond == NYR_MACH_COND_EQ || cond == NYR_MACH_COND_NE)) {
      if (!mach_rex(e, true, ra >= 8, false, ra >= 8) || !mach_u8(e, 0x85) ||
          !mach_u8(e, (unsigned char)(0xc0 | ((ra & 7) << 3) | (ra & 7))))
        return false;
    } else if (!mach_rex(e, true, false, false, ra >= 8) ||
               !mach_u8(e, small ? 0x83 : 0x81) ||
               !mach_u8(e, (unsigned char)(0xf8 | (ra & 7))) ||
               !(small ? mach_u8(e, (unsigned char)(ib & 0xff))
                       : mach_i32(e, (int32_t)ib)))
      return false;
  } else if (!mach_rex(e, true, rb >= 8, false, ra >= 8) || !mach_u8(e, 0x39) ||
             !mach_u8(e, (unsigned char)(0xc0 | ((rb & 7) << 3) | (ra & 7))))
    return false;
  /*
   * Not taken: jump over the early return into the normal prologue.
   */
  if (!mach_u8(e, (unsigned char)(0x70 | (cc ^ 1))) || !mach_u8(e, 0))
    return false;
  size_t patch = e->code.len - 1;
  if (rret >= 0) {
    if (!mach_mov_preg_preg(e, 0, rret))
      return false;
  } else if (!mach_mov_preg_imm(e, 0, rimm)) {
    return false;
  }
  if (!mach_emit_return(e))
    return false;
  size_t dist = e->code.len - (patch + 1);
  if (dist > 127 || !e->code.data)
    return mach_err(e, "x64 machine form encode: early exit too long");
  e->code.data[patch] = (unsigned char)dist;
  return true;
}

/*
 * `add(shl(x, k), y)` with k in 1..3, the SHL immediately before the ADD and
 * its result used only there, is one `lea dst, [y + x*2^k]` (radix/bit
 * accumulation `node = node * 2 + bit`). Decided at the SHL, which is then
 * not emitted; the ADD consumes the recorded index register.
 * NYTRIX_NO_SHL_ADD_LEA=1 disables it.
 */
static bool mach_shl_add_fuse_begin(nyr_x64_mach_enc_t *e,
                                    const nyr_mach_inst_t *shl) {
  e->shl_fuse_valid = false;
  const nyr_mach_func_t *m = e->mach;
  size_t i = e->cur_inst;
  if (!m || e->force_stack_reads || i + 1 >= m->inst_len ||
      shl->opcode != NYR_MACH_SHL || shl->src1.kind != NYR_MACH_OPERAND_IMM ||
      shl->src1.as.imm < 1 || shl->src1.as.imm > 3 ||
      shl->src0.kind != NYR_MACH_OPERAND_VREG ||
      shl->dst.kind != NYR_MACH_OPERAND_VREG ||
      shl->dst.as.reg >= m->vreg_len || !mach_is_i64(m, &shl->dst) ||
      !mach_is_i64(m, &shl->src0) || getenv("NYTRIX_NO_SHL_ADD_LEA"))
    return false;
  const nyr_mach_inst_t *add = &m->insts[i + 1];
  if (add->opcode != NYR_MACH_ADD || add->dst.kind != NYR_MACH_OPERAND_VREG ||
      !mach_is_i64(m, &add->dst) ||
      mach_block_for_inst(e, i) != mach_block_for_inst(e, i + 1) ||
      mach_inst_input_uses_vreg(add, shl->dst.as.reg) != 1 ||
      mach_inst_has_preg(add) || mach_inst_has_preg(shl))
    return false;
  const nyr_mach_operand_t *other =
      mach_op_is_vreg(&add->src0, shl->dst.as.reg) ? &add->src1 : &add->src0;
  if (other->kind != NYR_MACH_OPERAND_VREG || !mach_is_i64(m, other))
    return false;
  uint32_t d = shl->dst.as.reg;
  if (!e->def_count || e->def_count[d] != 1)
    return false;
  size_t uses = 0;
  for (size_t k = 0; k < m->inst_len && uses < 2; ++k) {
    const nyr_mach_inst_t *in = &m->insts[k];
    uses += mach_inst_input_uses_vreg(in, d);
    if (in->opcode == NYR_MACH_STORE && mach_op_is_vreg(&in->dst, d))
      uses += 2;
  }
  if (uses != 1)
    return false;
  if (!mach_seed_colored_preg(e, shl->src0.as.reg))
    return false;
  int x = mach_vreg_preg(e, shl->src0.as.reg);
  if (x < 0 || x == 4)
    return false;
  e->shl_fuse_valid = true;
  e->shl_fuse_add = i + 1;
  e->shl_fuse_index = x;
  e->shl_fuse_scale = 1u << (unsigned)shl->src1.as.imm;
  e->shl_fuse_dst = d;
  return true;
}

static bool mach_shl_add_fuse_emit(nyr_x64_mach_enc_t *e,
                                   const nyr_mach_inst_t *add, int dst_off) {
  e->shl_fuse_valid = false;
  const nyr_mach_operand_t *other =
      mach_op_is_vreg(&add->src0, e->shl_fuse_dst) ? &add->src1 : &add->src0;
  int base = -1;
  if (mach_seed_colored_preg(e, other->as.reg))
    base = mach_vreg_preg(e, other->as.reg);
  if (base < 0) {
    if (!mach_load_op_rax(e, other))
      return false;
    base = 0;
  }
  int dp = mach_vreg_def_preg(e, add->dst.as.reg);
  ny_x64_mem_addr_t sum = {(unsigned)base, e->shl_fuse_index, e->shl_fuse_scale,
                           0};
  int target = dp >= 0 ? dp : 0;
  if (!mach_rex_for_addr(e, &sum, true, target >= 8) || !mach_u8(e, 0x8d) ||
      !mach_emit_mem(e, &sum, (unsigned)target))
    return false;
  if (target == 10)
    mach_r10_index_invalidate(e);
  if (dp >= 0)
    return mach_commit_preg(e, &add->dst, dst_off, (unsigned)dp, false);
  mach_rax_invalidate(e);
  return mach_commit_vreg(e, &add->dst, dst_off);
}

static bool mach_spill_params(nyr_x64_mach_enc_t *e) {
  const nyr_mach_func_t *mach = e->mach;
  if (!mach->frame_slot_len)
    return true;
  bool *params = calloc(mach->frame_slot_len, sizeof(*params));
  if (!params)
    return mach_err(e, "x64 machine form encode: OOM");
  if (!nyr_mach_param_slots(mach, params)) {
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
    if (mach->param_types && slot < mach->param_count &&
        mach->param_types[slot] == NYIR_PARAM_AGGREGATE) {
      nyr_mach_sysv_aggregate_t aggregate;
      size_t gp_first = gp_arg, fp_first = fp_arg;
      if (is_win || !mach->param_abi ||
          !nyr_mach_sysv_aggregate_parameter(
              mach->param_abi[slot], e->target->gp_arg_reg_count,
              e->target->fp_arg_reg_count, &gp_arg, &fp_arg, &aggregate)) {
        free(params);
        return mach_err(
            e, "x64 machine form encode: unsupported aggregate parameter");
      }
      int home = 0;
      if (aggregate.in_registers) {
        if (!mach->param_aggregate_home_slots ||
            mach->param_aggregate_home_slots[slot] == UINT32_MAX ||
            (size_t)mach->param_aggregate_home_slots[slot] + aggregate.chunks >
                mach->frame_slot_len) {
          free(params);
          return mach_err(e, "x64 machine form encode: invalid aggregate home");
        }
        home = e->frame_slot_offsets[mach->param_aggregate_home_slots[slot] +
                                     aggregate.chunks - 1];
        for (unsigned chunk = 0; chunk < aggregate.chunks; ++chunk) {
          unsigned cls = aggregate.classes[chunk];
          bool saved = cls == NYIR_ARG_CLASS_NONE ||
                       (cls == NYIR_ARG_CLASS_INTEGER
                            ? mach_store_gpr(e, home + (int)chunk * 8,
                                             gp_regs[gp_first++])
                            : mach_store_xmm(e, home + (int)chunk * 8, false,
                                             (unsigned)fp_first++));
          if (!saved) {
            free(params);
            return false;
          }
        }
      } else {
        home = 16 + (int)stack_arg * 8;
        stack_arg += aggregate.chunks;
      }
      if (!mach_rex(e, true, false, false, false) || !mach_u8(e, 0x8d) ||
          !mach_rbp_disp(e, 0, home) ||
          !mach_store_rax(e, e->frame_slot_offsets[slot])) {
        free(params);
        return false;
      }
      continue;
    }
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
      /*
       * The entry LOAD copies this argument straight into its assigned preg.
       */
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
 * SSE scalar op xmm0, mem: F2/F3 0F opc /r — opc: 58 add 5c sub 59 mul 5e
 * div. The ModRM comes from mach_rbp_disp so a home inside disp8 range costs
 * five bytes instead of eight.
 */
static bool mach_sse_op_mem(nyr_x64_mach_enc_t *e, int off, bool f32,
                            unsigned opc) {
  return mach_u8(e, f32 ? 0xf3 : 0xf2) && mach_u8(e, 0x0f) && mach_u8(e, opc) &&
         mach_rbp_disp(e, 0, off);
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
  if (mach->vreg_len && !nyr_mach_regalloc_build_x64_class(
                            mach, NYR_MACH_REGCLASS_GPR, gp_color_count, &a))
    return false;
  nyr_mach_regalloc_summarize(mach, &a, mach->inst_len, gpr);
  nyr_mach_regalloc_free(&a);

  bool fp_eligible = mach_float_fast_eligible(mach);
  for (size_t i = 0; fp_eligible && i < mach->inst_len; ++i)
    if (mach->insts[i].opcode == NYR_MACH_SIN ||
        mach->insts[i].opcode == NYR_MACH_COS)
      fp_eligible = false;
  if (mach->vreg_len && fp_eligible) {
    if (!nyr_mach_regalloc_build_x64_class(mach, NYR_MACH_REGCLASS_FPR,
                                           MFP_COLOR_N, &a)) {
      nyr_mach_regalloc_free(&a);
      return false;
    }
    nyr_mach_regalloc_summarize(mach, &a, mach->inst_len, fpr);
    nyr_mach_regalloc_free(&a);
  }

  if (mach->vreg_len && mach_vector_fast_eligible(mach)) {
    if (!nyr_mach_regalloc_build_class(mach, NYR_MACH_REGCLASS_VECTOR,
                                       MVEC_COLOR_N, &a)) {
      nyr_mach_regalloc_free(&a);
      return false;
    }
    nyr_mach_regalloc_summarize(mach, &a, mach->inst_len, vector);
    nyr_mach_regalloc_free(&a);
  }
  return true;
}

static bool mach_operand_is_v256(const nyr_mach_func_t *m,
                                 const nyr_mach_operand_t *op) {
  if (!op || op->kind != NYR_MACH_OPERAND_VREG || !m->vreg_types ||
      op->as.reg >= m->vreg_len)
    return false;
  nyr_mach_type_t t = m->vreg_types[op->as.reg];
  return t == NYR_MACH_TYPE_V256_I64 || t == NYR_MACH_TYPE_V256_F64;
}

static bool mach_inst_touches_v256(const nyr_mach_func_t *m,
                                   const nyr_mach_inst_t *in) {
  if (mach_operand_is_v256(m, &in->dst) || mach_operand_is_v256(m, &in->src0) ||
      mach_operand_is_v256(m, &in->src1) || mach_operand_is_v256(m, &in->src2))
    return true;
  for (size_t a = 0; in->args && a < in->args_len; ++a)
    if (mach_operand_is_v256(m, &in->args[a]))
      return true;
  return false;
}

static bool mach_block_has_scalar_float(const nyr_mach_func_t *mach,
                                        size_t bi) {
  const nyr_mach_block_t *blk = &mach->blocks[bi];
  for (size_t n = 0; n < blk->inst_count; ++n) {
    const nyr_mach_inst_t *in = &mach->insts[blk->first_inst + n];
    if (in->opcode == NYR_MACH_CALL)
      continue; /* the call site clears the state itself */
    if (mach_is_float(mach, &in->dst) || mach_is_float(mach, &in->src0) ||
        mach_is_float(mach, &in->src1) || mach_is_float(mach, &in->src2))
      return true;
  }
  return false;
}

static bool mach_block_v256_live_in(const nyr_x64_mach_enc_t *e, size_t bi) {
  const nyr_mach_func_t *mach = e->mach;
  if (!e->vec_fast_path || !e->vec_regalloc.live_in)
    return false;
  for (uint32_t v = 0; v < mach->vreg_len; ++v) {
    nyr_mach_type_t t = mach->vreg_types[v];
    if ((t == NYR_MACH_TYPE_V256_I64 || t == NYR_MACH_TYPE_V256_F64) &&
        nyr_mach_regalloc_live_in(&e->vec_regalloc, bi, v))
      return true;
  }
  return false;
}

/*
 * Forward dataflow over the machine blocks: the upper YMM state is dirty
 * after an instruction with a 256-bit operand and clean after a call (the
 * call site clears it, and callees return it clean) or after the entry
 * vzeroupper this encoder emits. A block entered only with clean state needs
 * no vzeroupper of its own, which keeps the instruction out of scalar loops
 * that merely share a function with a vector loop. Every unknown is resolved
 * toward dirty, so the result only ever adds a vzeroupper.
 */
static bool mach_build_ymm_dirty_in(nyr_x64_mach_enc_t *e) {
  const nyr_mach_func_t *mach = e->mach;
  size_t nb = mach->block_len;
  enum { YMM_KEEP = 0, YMM_CLEAN = 1, YMM_DIRTY = 2 };
  unsigned char *dirty_in = calloc(nb ? nb : 1, 1);
  unsigned char *last = calloc(nb ? nb : 1, 1);
  unsigned char *clears = calloc(nb ? nb : 1, 1);
  if (!dirty_in || !last || !clears) {
    free(dirty_in);
    free(last);
    free(clears);
    return false;
  }
  for (size_t b = 0; b < nb; ++b) {
    const nyr_mach_block_t *blk = &mach->blocks[b];
    for (size_t n = 0; n < blk->inst_count; ++n) {
      const nyr_mach_inst_t *in = &mach->insts[blk->first_inst + n];
      /*
       * A callee returning a 256-bit value leaves the upper state dirty.
       */
      if (in->opcode == NYR_MACH_CALL && !mach_operand_is_v256(mach, &in->dst))
        last[b] = YMM_CLEAN;
      else if (mach_inst_touches_v256(mach, in))
        last[b] = YMM_DIRTY;
    }
    clears[b] =
        mach_block_has_scalar_float(mach, b) && !mach_block_v256_live_in(e, b);
  }
  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t b = 0; b < nb; ++b) {
      bool out = last[b] == YMM_DIRTY ||
                 (last[b] == YMM_KEEP && dirty_in[b] && !clears[b]);
      if (!out)
        continue;
      const nyr_mach_block_t *blk = &mach->blocks[b];
      bool falls = true;
      for (size_t n = 0; n < blk->inst_count; ++n) {
        const nyr_mach_inst_t *in = &mach->insts[blk->first_inst + n];
        const nyr_mach_operand_t *ops[] = {&in->dst, &in->src0, &in->src1,
                                           &in->src2};
        for (size_t k = 0; k < 4 + in->args_len; ++k) {
          const nyr_mach_operand_t *op = k < 4 ? ops[k] : &in->args[k - 4];
          if (op->kind != NYR_MACH_OPERAND_BLOCK)
            continue;
          size_t t = op->as.block_index;
          if (t < nb && !dirty_in[t]) {
            dirty_in[t] = 1;
            changed = true;
          }
        }
        if (n + 1 == blk->inst_count)
          falls = in->opcode != NYR_MACH_BR && in->opcode != NYR_MACH_RET &&
                  in->opcode != NYR_MACH_TRAP;
      }
      if (falls && b + 1 < nb && !dirty_in[b + 1]) {
        dirty_in[b + 1] = 1;
        changed = true;
      }
    }
  }
  free(last);
  free(clears);
  e->ymm_dirty_in = dirty_in;
  return true;
}

/*
 * At the entry of a block that runs scalar-float code, clear the upper YMM
 * state when it may be dirty there and no 256-bit value can be resident
 * (none is live into the block). Blocks without float work keep their vector
 * state untouched.
 */
static bool mach_block_vzeroupper(nyr_x64_mach_enc_t *e, size_t bi) {
  const nyr_mach_func_t *mach = e->mach;
  if (!mach_block_has_scalar_float(mach, bi))
    return true;
  if (mach_block_v256_live_in(e, bi))
    return true;
  if (!e->ymm_dirty_in && !mach_build_ymm_dirty_in(e))
    return mach_err(e, "x64 machine form encode: OOM (ymm state)");
  if (!e->ymm_dirty_in[bi])
    return true;
  return mach_vzeroupper(e);
}

static size_t mach_without_jumps(size_t off, const size_t *cuts, size_t count) {
  if (off == SIZE_MAX)
    return off;
  size_t lo = 0, hi = count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (cuts[mid] + 5 <= off)
      lo = mid + 1;
    else
      hi = mid;
  }
  if (lo < count && cuts[lo] < off)
    off = cuts[lo];
  return off - 5 * lo;
}

/*
 * Fold jcc taken / jmp other / taken: into inverse-jcc other / taken:.
 * Check the emitted bytes as well as the CFG: segment-end home stores or
 * vector boundary code must never be skipped. A trampoline with another
 * predecessor must retain its jump. Do this before patching block branches,
 * and move relocations and MIR source offsets with the removed bytes.
 */
static bool mach_fold_branch_trampolines(nyr_x64_mach_enc_t *e) {
  if (e->patch_count < 2)
    return true;
  const nyr_mach_func_t *m = e->mach;
  size_t *cuts = malloc(e->patch_count * sizeof(*cuts));
  if (!cuts)
    return mach_err(e, "x64 branch folding: OOM");
  size_t count = 0;
  for (size_t i = 0; i + 1 < e->patch_count; ++i) {
    size_t at = e->patch_at[i];
    uint32_t taken = e->patch_block[i];
    if (at == SIZE_MAX || at < 2 || at + 9 > e->code.len || taken < 2 ||
        taken >= m->block_len || e->code.data[at - 2] != 0x0f ||
        (e->code.data[at - 1] & 0xf0) != 0x80 || e->code.data[at + 4] != 0xe9 ||
        e->patch_at[i + 1] != at + 5 || e->block_off[taken - 1] != at + 4 ||
        e->block_off[taken] != at + 9)
      continue;
    const nyr_mach_block_t *source = &m->blocks[taken - 2];
    const nyr_mach_block_t *trampoline = &m->blocks[taken - 1];
    if (!source->inst_count || trampoline->inst_count != 1 ||
        m->insts[source->first_inst + source->inst_count - 1].opcode !=
            NYR_MACH_BR_IF ||
        m->insts[trampoline->first_inst].opcode != NYR_MACH_BR ||
        !mach_block_has_only_pred(m, taken - 1, taken - 2))
      continue;
    e->code.data[at - 1] ^= 1;
    e->patch_block[i] = e->patch_block[i + 1];
    cuts[count++] = at + 4;
    e->patch_at[++i] = SIZE_MAX;
  }
  if (count) {
    size_t read = 0, write = 0;
    for (size_t i = 0; i < count; ++i) {
      size_t len = cuts[i] - read;
      memmove(e->code.data + write, e->code.data + read, len);
      write += len;
      read = cuts[i] + 5;
    }
    memmove(e->code.data + write, e->code.data + read, e->code.len - read);
    e->code.len -= 5 * count;
    for (size_t b = 0; b < m->block_len; ++b)
      e->block_off[b] = mach_without_jumps(e->block_off[b], cuts, count);
    for (size_t i = 0; i < m->inst_len; ++i)
      e->inst_offsets[i] = mach_without_jumps(e->inst_offsets[i], cuts, count);
    for (size_t i = 0; i < e->reloc_count; ++i)
      e->relocs[i].disp_off =
          mach_without_jumps(e->relocs[i].disp_off, cuts, count);
    if (e->bare_payload_entry)
      e->bare_payload_entry =
          mach_without_jumps(e->bare_payload_entry - 1, cuts, count) + 1;
    size_t kept = 0;
    for (size_t i = 0; i < e->patch_count; ++i) {
      if (e->patch_at[i] == SIZE_MAX)
        continue;
      e->patch_at[kept] = mach_without_jumps(e->patch_at[i], cuts, count);
      e->patch_short[kept] = e->patch_short[i];
      e->patch_block[kept++] = e->patch_block[i];
    }
    e->patch_count = kept;
  }
  free(cuts);
  return true;
}

#include "code/backend/mach/arch/x86/object/features.h"
#include "code/backend/mach/arch/x86/object/loop_align.h"
#include "code/backend/mach/arch/x86/object/regalloc.h"

/*
 * Per-vreg operand reference counts, built once per function.  The encoder
 * uses them to prove that an address materialization has a single consumer
 * before folding it into that consumer's destination register.
 */
static bool mach_build_vreg_use_counts(nyr_x64_mach_enc_t *e) {
  if (!e || !e->mach)
    return false;
  const nyr_mach_func_t *m = e->mach;
  free(e->vreg_use_count);
  e->vreg_use_count = NULL;
  if (!m->vreg_len)
    return true;
  uint32_t *counts = calloc(m->vreg_len, sizeof(*counts));
  if (!counts)
    return false;
  for (size_t i = 0; i < m->inst_len; ++i) {
    const nyr_mach_inst_t *in = &m->insts[i];
    const nyr_mach_operand_t *ops[] = {&in->src0, &in->src1, &in->src2};
    for (size_t k = 0; k < sizeof(ops) / sizeof(ops[0]); ++k) {
      if (ops[k]->kind != NYR_MACH_OPERAND_VREG ||
          ops[k]->as.reg >= m->vreg_len)
        continue;
      if (counts[ops[k]->as.reg] < 3)
        counts[ops[k]->as.reg]++;
    }
  }
  e->vreg_use_count = counts;
  return true;
}

/*
 * Missed-optimization audit for one function's machine form.
 *
 * Counts the patterns this backend can still improve, in exactly the terms the
 * encoder controls: address materializations that admit a fold, copies whose
 * source and destination already share a color, adjacent store/load pairs
 * through one home, unconditional edges to the next block, and homes beyond
 * disp8 reach with their reference count.  NYTRIX_AUDIT=1 prints one stable
 * line per function, so a change can be judged by the same counters that
 * motivate it rather than by looking at assembly.
 */
static void mach_audit_function(const nyr_x64_mach_enc_t *e, const char *name) {
  const char *env = getenv("NYTRIX_AUDIT");
  if (!env || !*env || *env == '0')
    return;
  const nyr_mach_func_t *m = e->mach;
  if (!m)
    return;
  unsigned lea_copy = 0, self_move = 0, spill_pair = 0, fallthrough = 0;
  size_t bi = 0;
  for (size_t i = 0; i < m->inst_len; ++i) {
    while (bi < m->block_len &&
           i >= m->blocks[bi].first_inst + m->blocks[bi].inst_count)
      ++bi;
    const nyr_mach_inst_t *in = &m->insts[i];
    if (in->opcode == NYR_MACH_LEA && in->dst.kind == NYR_MACH_OPERAND_VREG &&
        in->src0.kind == NYR_MACH_OPERAND_SYMBOL && in->src0.as.symbol &&
        e->vreg_use_count && in->dst.as.reg < m->vreg_len &&
        e->vreg_use_count[in->dst.as.reg] == 1 && i + 1 < m->inst_len &&
        m->insts[i + 1].opcode == NYR_MACH_COPY &&
        m->insts[i + 1].src0.kind == NYR_MACH_OPERAND_VREG &&
        m->insts[i + 1].src0.as.reg == in->dst.as.reg)
      ++lea_copy;
    if (in->opcode == NYR_MACH_COPY && in->src0.kind == NYR_MACH_OPERAND_VREG &&
        in->dst.kind == NYR_MACH_OPERAND_VREG &&
        in->src0.as.reg != in->dst.as.reg && in->src0.as.reg < m->vreg_len &&
        in->dst.as.reg < m->vreg_len) {
      int sp = mach_vreg_def_preg(e, in->src0.as.reg);
      int dp = mach_vreg_def_preg(e, in->dst.as.reg);
      if (sp >= 0 && sp == dp)
        ++self_move;
    }
    if (in->opcode == NYR_MACH_STORE && i + 1 < m->inst_len &&
        m->insts[i + 1].opcode == NYR_MACH_LOAD &&
        in->dst.kind == NYR_MACH_OPERAND_FRAME &&
        m->insts[i + 1].src0.kind == NYR_MACH_OPERAND_FRAME &&
        mach_slot_off(e, &in->dst) == mach_slot_off(e, &m->insts[i + 1].src0))
      ++spill_pair;
    /*
     * Unconditional edge to the immediately following block: the encoder
     * elides the jmp and falls through, so this counts a live optimization
     * rather than a miss.  A nonzero count that ever appears in the emitted
     * bytes would mean the elision stopped applying.
     */
    if (in->opcode == NYR_MACH_BR && in->src1.kind == NYR_MACH_OPERAND_BLOCK &&
        bi + 1 < m->block_len && in->src1.as.block_index == bi + 1)
      ++fallthrough;
  }
  /*
   * Register-allocation shape for this function, taken from the allocator's
   * own result rather than recomputed: how many live segments it produced, how
   * many it had to spill, and how many physical colors it actually used.  A
   * size or copy-count change can then be attributed to allocation instead of
   * to the encoder.
   */
  unsigned ra_segments = 0, ra_spilled = 0, ra_reload = 0, ra_colors = 0;
  unsigned ra_spill_live = 0, ra_spill_dying = 0;
  if (e->regalloc.segments) {
    uint64_t seen = 0;
    ra_segments = (unsigned)e->regalloc.segment_len;
    for (size_t i = 0; i < e->regalloc.segment_len; ++i) {
      const nyr_mach_live_segment_t *seg = &e->regalloc.segments[i];
      if (seg->spill) {
        ++ra_spilled;
        /*
         * Only a spill that is reloaded later costs a store and a load; a
         * spilled value that dies in the segment needs no load at all.
         * Splitting the count says whether `ra_spilled` is real pressure or
         * bookkeeping. Home reachability cannot be read here: `vreg_home_offs`
         * is only populated for the home-DSE path, so a home-based count would
         * report every spill as home-less.
         */
        if (seg->reload)
          ++ra_spill_live;
        else
          ++ra_spill_dying;
      }
      if (seg->reload)
        ++ra_reload;
      if (seg->color >= 0 && seg->color < 64)
        seen |= UINT64_C(1) << seg->color;
    }
    ra_colors = (unsigned)__builtin_popcountll((unsigned long long)seen);
  }
  fprintf(stderr,
          "[audit] fn=%s lea_copy=%u self_move=%u spill_pair=%u "
          "fallthrough_elided=%u lea_color=%u lea_staged=%u "
          "ra_segments=%u ra_spilled=%u ra_reload=%u ra_colors=%u "
          "ra_spill_live=%u ra_spill_dying=%u\n",
          name && *name ? name : "fn", lea_copy, self_move, spill_pair,
          fallthrough, e->stat_lea_color, e->stat_lea_staged, ra_segments,
          ra_spilled, ra_reload, ra_colors, ra_spill_live, ra_spill_dying);
}

static bool mach_encode_function(nyr_x64_mach_enc_t *e, const char *name,
                                 bool tag_return) {
  (void)tag_return;
  e->debug_name = name;
  e->stat_lea_color = 0;
  e->stat_lea_staged = 0;
  const nyr_mach_func_t *mach = e->mach;
  if (!mach_validate_target_features(e))
    return false;
  if (mach->x64_endbr &&
      !mach_bytes(e, (const unsigned char[]){0xf3, 0x0f, 0x1e, 0xfa}, 4))
    return false;
  e->inst_offsets =
      calloc(mach->inst_len ? mach->inst_len : 1, sizeof(*e->inst_offsets));
  if (!e->inst_offsets && mach->inst_len)
    return mach_err(e, "x64 machine form encode: OOM (source map)");
  for (size_t i = 0; i < mach->inst_len; ++i)
    e->inst_offsets[i] = SIZE_MAX;
  bool vector_fast_eligible = mach_configure_function_features(e);
  e->last_rax_valid = false;
  e->last_rax_off = 0;
  e->last_rax_vreg = -1;
  unsigned save_mask = 0;
  bool unknown_memory = mach_has_unknown_memory_effect(mach);
  bool private_spill_homes = !unknown_memory && e->target &&
                             e->target->abi == NY_NATIVE_ABI_SYSV &&
                             mach_spill_homes_are_private(mach);
  e->regalloc = (nyr_mach_regalloc_t){0};
  e->fp_regalloc = (nyr_mach_regalloc_t){0};
  e->vec_regalloc = (nyr_mach_regalloc_t){0};
  e->color_seeded = NULL;
  e->fp_color_seeded = NULL;
  e->vec_color_seeded = NULL;
  e->last_home_store_end = 0;
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
    bool alloc_ok = nyr_mach_regalloc_build_x64_class(
        e->mach, NYR_MACH_REGCLASS_GPR, gp_color_count, &e->regalloc);
    if (alloc_ok) {
      mach_decolor_literals(e, e->mach);
      mach_allocate_loop_gprs(e, name, private_spill_homes);
      /*
       * With loop registers the eviction pass recolors under their
       * reservations; without any, nothing is reserved.
       */
      if (!e->loop_gpr)
        mach_recolor_local_spills(e, e->mach);
    }
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
      fprintf(stderr, "[x64-regalloc] vector-fast-eligible=%s active=%s\n",
              vector_fast_eligible ? "yes" : "no",
              e->vec_fast_path ? "yes" : "no");
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
  mach_try_build_fast_regalloc(mach, &e->fp_fast_path, &e->fp_color_seeded,
                               fp_color_seeded_stack, &e->fp_colors_len,
                               e->fp_color_limit, &e->fp_regalloc, false);
  free(e->fp_seed_inst);
  free(e->fp_last_def_inst);
  e->fp_seed_inst = e->fp_last_def_inst = NULL;
  if (e->fp_fast_path && e->fp_color_seeded && mach->vreg_len) {
    e->fp_seed_inst = calloc(mach->vreg_len, sizeof(*e->fp_seed_inst));
    e->fp_last_def_inst = calloc(mach->vreg_len, sizeof(*e->fp_last_def_inst));
    if (!e->fp_seed_inst || !e->fp_last_def_inst) {
      free(e->fp_seed_inst);
      free(e->fp_last_def_inst);
      e->fp_seed_inst = e->fp_last_def_inst = NULL;
    }
  }
  mach_allocate_loop_fprs(e, name);
  /*
   * Scalar floats normally reserve xmm4..xmm7 (MVEC_COLOR_N). When the float
   * allocation and the loop-pinned floats stayed in xmm0..xmm3, vectors may
   * use xmm4..xmm7 as in a float-free function.
   */
  if (e->vec_fast_path && e->vec_color_n == MVEC_COLOR_N && e->target &&
      e->target->abi == NY_NATIVE_ABI_SYSV &&
      (e->target->caps & NY_NATIVE_CAP_MACH_AVX2) && e->fp_fast_path &&
      !getenv("NYTRIX_NO_SHARED_XMM4_7")) {
    bool low_fprs = true;
    for (size_t i = 0; low_fprs && i < e->fp_regalloc.segment_len; ++i)
      low_fprs = e->fp_regalloc.segments[i].color < MVEC_WIDE_PREG_BASE;
    for (size_t v = 0; low_fprs && e->loop_fpr && v < e->fp_colors_len; ++v)
      low_fprs = e->loop_fpr[v] < (int)MVEC_WIDE_PREG_BASE;
    if (low_fprs)
      e->vec_color_n = MVEC_COLOR_WIDE_N;
  }
  mach_try_build_fast_regalloc(mach, &e->vec_fast_path, &e->vec_color_seeded,
                               vec_color_seeded_stack, &e->vec_colors_len,
                               e->vec_color_n, &e->vec_regalloc, true);
  /*
   * Vector scalar scratch clobbers are represented in the GPR allocator;
   * vector values still use their separate XMM/YMM color bank.
   * Calls and fixed-register sequences publish resident scalar values to
   * canonical homes and invalidate their residency before clobbering.
   */
  size_t call_count = 0;
  bool has_inline_asm = false;
  for (size_t i = 0; i < mach->inst_len; ++i) {
    if (mach->insts[i].opcode == NYR_MACH_CALL)
      ++call_count;
    has_inline_asm |= mach->insts[i].opcode == NYR_MACH_INLINE_ASM;
  }
  /*
   * Calls with stack-passed arguments do not invalidate unrelated register
   * ranges. Candidate ranges that cross a call are rejected by the allocator's
   * call-safety proof; values used as call arguments are read from their
   * current locations by call lowering. CFGs above the loop allocator's
   * 256-block proof limit still stay on homes.
   */
  bool env_forces_stack = getenv("NYTRIX_FORCE_STACK_GPR") != NULL;
  bool indirect_memory = mach_has_indirect_memory(mach);
  bool indirect_store = indirect_memory && mach_has_indirect_store(mach);
  /*
   * An opaque memory effect is not a register clobber. Calls are handled by
   * the callee-saved preservation path and inline asm is the only remaining
   * opaque use that needs every resident value in a canonical home.
   */
  bool inline_asm_needs_homes = has_inline_asm;
  const char *stack_reason = env_forces_stack         ? "environment"
                             : inline_asm_needs_homes ? "inline-asm"
                             : indirect_store && !private_spill_homes
                                 ? "indirect-store-live-ranges"
                             : mach->block_len > MACH_SET_BLOCKS ? "block-count"
                             : mach->vreg_len > MACH_LOOP_GPR_MAX_VREGS              ? "value-count"
                                                                 : "none";
  /*
   * Call count alone is not a reason to abandon register homes: a function
   * can have many calls in cold setup/reporting code while its hot loops are
   * call-free. The block/value caps below still bound the whole-function
   * edge proofs for genuinely large bodies.
   */
  e->force_stack_reads = env_forces_stack || inline_asm_needs_homes ||
                         mach->block_len > MACH_SET_BLOCKS ||
                         mach->vreg_len > MACH_LOOP_GPR_MAX_VREGS;
  if (indirect_store && !private_spill_homes && !e->force_stack_reads) {
    bool kept = mach_keep_homes_for_store_live_values(e);
    if (!kept)
      e->force_stack_reads = true;
  }
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
   * Match the allocator's explicit GPR carry opt-in.
   */
  const char *loop_carry_env = getenv("NYTRIX_GPR_LOOP_CARRY");
  e->loop_carry_seeding = loop_carry_env && strcmp(loop_carry_env, "0") != 0 &&
                          strcmp(loop_carry_env, "off") != 0 &&
                          strcmp(loop_carry_env, "false") != 0;
  const char *no_same_color = getenv("NYTRIX_NO_SAME_COLOR_COPY");
  e->same_color_copy_fold =
      !(no_same_color && strcmp(no_same_color, "0") != 0 &&
        strcmp(no_same_color, "off") != 0);
  const char *no_lea_color = getenv("NYTRIX_NO_LEA_COLOR");
  e->lea_assign_color = !(no_lea_color && strcmp(no_lea_color, "0") != 0 &&
                          strcmp(no_lea_color, "off") != 0);
  /*
   * The vector allocator enables only chains that pass the all-edge proof;
   * its opt-out simply leaves every vector segment home-backed.
   */
  e->vector_loop_carry_seeding = true;

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
  if (mach->frame_slot_len >= SIZE_MAX / sizeof(*e->frame_slot_offsets))
    return mach_err(e, "x64 machine form encode: frame slot overflow");
  e->frame_slot_offsets =
      malloc((mach->frame_slot_len + 1) * sizeof(*e->frame_slot_offsets));
  if (!e->frame_slot_offsets)
    return mach_err(e, "x64 machine form encode: OOM (frame offsets)");
  for (size_t slot = 0; slot <= mach->frame_slot_len; ++slot) {
    size_t base = (size_t)e->cs_slots * 8u;
    if (frame_slots_bytes > (size_t)INT_MAX - 8u - base)
      return mach_err(e, "x64 machine form encode: frame slot overflow");
    e->frame_slot_offsets[slot] = -(int)(base + frame_slots_bytes + 8u);
    if (slot == mach->frame_slot_len)
      break;
    size_t ssz = mach->frame_slots[slot].size;
    size_t salign = mach->frame_slots[slot].align;
    if (ssz < 8)
      ssz = 8;
    if (!salign)
      salign = 1;
    if (frame_slots_bytes > SIZE_MAX - (salign - 1))
      return mach_err(e, "x64 machine form encode: frame slot overflow");
    size_t aligned = (frame_slots_bytes + salign - 1) & ~(salign - 1);
    if (aligned > SIZE_MAX - ssz)
      return mach_err(e, "x64 machine form encode: frame slot overflow");
    frame_slots_bytes = aligned + ssz;
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
      if (nyr_mach_profile_hot_loop_block(mach, seg->block)) {
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
              "reload=%d carried=%d live=%d→%d reason=%d\n",
              seg->vreg, seg->block, seg->start, seg->end, seg->color,
              (int)seg->spill, (int)seg->reload, (int)seg->carried,
              (int)nyr_mach_regalloc_live_in(&e->vec_regalloc, seg->block,
                                             seg->vreg),
              (int)nyr_mach_regalloc_live_out(&e->vec_regalloc, seg->block,
                                              seg->vreg),
              (int)seg->spill_reason);
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

  if (!mach_emit_early_exit(e, name))
    return false;
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
  if (base_name && strcmp(base_name, "main") == 0 && !mach->c_abi &&
      e->target && e->target->abi == NY_NATIVE_ABI_SYSV) {
    char set_args[256];
    snprintf(set_args, sizeof(set_args), "%s_ny_aot_set_args", symbol_prefix);
    if (!mach_u8(e, 0xe8) || !mach_add_reloc(e, set_args, NY_RELOC_PLT32) ||
        !mach_i32(e, 0))
      return false;
  }
  if (base_name && strcmp(base_name, "rt_main") != 0 &&
      (mach->c_abi || strcmp(base_name, "main") != 0) &&
      !mach_prepare_direct_params(e))
    return false;
  if (base_name && strcmp(base_name, "rt_main") != 0 &&
      (mach->c_abi || strcmp(base_name, "main") != 0) && !mach_spill_params(e))
    return false;

  if (mach->vreg_len) {
    e->def_inst = malloc(mach->vreg_len * sizeof(*e->def_inst));
    e->select_test_src = malloc(mach->vreg_len * sizeof(*e->select_test_src));
    e->select_test_equal =
        calloc(mach->vreg_len, sizeof(*e->select_test_equal));
    e->select_skip_alu = calloc(mach->vreg_len, sizeof(*e->select_skip_alu));
    e->select_cmp_inst = malloc(mach->vreg_len * sizeof(*e->select_cmp_inst));
    if (!e->def_inst || !e->select_test_src || !e->select_test_equal ||
        !e->cmp_elision_action || !e->select_skip_alu || !e->select_cmp_inst)
      return mach_err(e, "x64 machine form encode: OOM");
    for (size_t v = 0; v < mach->vreg_len; ++v) {
      e->def_inst[v] = SIZE_MAX;
      e->select_test_src[v] = -1;
      e->select_cmp_inst[v] = SIZE_MAX;
    }
    e->def_count = calloc(mach->vreg_len, sizeof(*e->def_count));
    e->vec_home_stored = calloc(mach->vreg_len, sizeof(*e->vec_home_stored));
    free(e->gpr_home_stored);
    e->gpr_home_stored = calloc(mach->vreg_len, sizeof(*e->gpr_home_stored));
    if (!e->def_count || !e->vec_home_stored || !e->gpr_home_stored)
      return mach_err(e, "x64 machine form encode: OOM");
    for (size_t i = 0; i < mach->inst_len; ++i) {
      const nyr_mach_inst_t *mi = &mach->insts[i];
      if (mi->opcode != NYR_MACH_STORE &&
          mi->dst.kind == NYR_MACH_OPERAND_VREG &&
          mi->dst.as.reg < mach->vreg_len) {
        e->def_inst[mi->dst.as.reg] = i;
        if (e->def_count[mi->dst.as.reg] < 2)
          e->def_count[mi->dst.as.reg]++;
      }
    }
    /*
     * Entry-block LEAs dominate all uses, but the vreg-numbered address map
     * is only sound when that vreg has no other definition.  Machine PHIs
     * can have edge-copy definitions that reuse the destination vreg, so
     * checking the entry LEA alone can rematerialize the wrong address.
     */
    if (mach->block_len > 0 && mach->inst_len > 0) {
      const nyr_mach_block_t *aentry = &mach->blocks[0];
      const size_t aend = aentry->first_inst + aentry->inst_count;
      for (size_t i = aentry->first_inst; i < aend && i < mach->inst_len; ++i) {
        const nyr_mach_inst_t *ci = &mach->insts[i];
        if (ci->opcode == NYR_MACH_LEA &&
            ci->dst.kind == NYR_MACH_OPERAND_VREG &&
            ci->dst.as.reg < mach->vreg_len &&
            e->def_count[ci->dst.as.reg] == 1 &&
            (ci->src0.kind == NYR_MACH_OPERAND_FRAME ||
             ci->src0.kind == NYR_MACH_OPERAND_SYMBOL)) {
          addr_fold[ci->dst.as.reg] = ci->src0;
          addr_fold_known[ci->dst.as.reg] = true;
        }
      }
    }
    if (!mach_prepare_select_bit_test(e, name))
      return mach_err(e, "x64 machine form encode: select fusion failed");
  }
  if (!mach_build_vreg_block_use(e))
    return false;
  if (!mach_build_literal_use_map(e))
    return mach_err(e, "x64 machine form encode: OOM (literal use map)");
  if (!mach_build_vreg_use_counts(e))
    return mach_err(e, "x64 machine form encode: OOM (vreg use counts)");
  if (!mach_build_block_align(e))
    return mach_err(e, "x64 machine form encode: OOM (block alignment)");
  e->block_off = calloc(mach->block_len ? mach->block_len : 1, sizeof(size_t));
  if (!e->block_off && mach->block_len)
    return mach_err(e, "x64 machine form encode: OOM");
  for (size_t bi = 0; bi < mach->block_len; ++bi) {
    e->last_home_store_end = 0; /* A predecessor may bypass the prior store. */
    e->cur_block = bi;
    e->cur_block_valid = true;
    if (e->block_align && e->block_align[bi] &&
        !mach_emit_align_nops(e, e->block_align[bi]))
      return false;
    e->block_off[bi] = e->code.len;
    const nyr_mach_block_t *blk = &mach->blocks[bi];
    if (e->uses_ymm && !mach_block_vzeroupper(e, bi))
      return false;
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
      if (e->vector_loop_carry_seeding)
        for (size_t si = 0; si < e->vec_regalloc.segment_len; ++si) {
          const nyr_mach_live_segment_t *seg = &e->vec_regalloc.segments[si];
          /*
           * A carried segment may also be marked spill: the allocator's exit
           * homing keeps `carried` and adds `spill` when a successor leaves
           * the loop. The common preg is still resident at block entry (the
           * begin-segment hook relies on that), and the branch/end-of-block
           * flush then writes the home for the exit edge. Requiring !spill
           * here unseeded the accumulator, so that flush never ran and a
           * `vec8.reduce_add` after a zero-trip vector loop read an unwritten
           * home (`result += invariant` for 3 iterations gave 2491, not 2451).
           */
          if (seg->block == bi && seg->carried && seg->color >= 0 &&
              (!seg->spill || seg->start == blk->first_inst) &&
              seg->vreg < e->vec_colors_len)
            e->vec_color_seeded[seg->vreg] = true;
        }
    }
    if (e->loop_fpr)
      for (uint32_t v = 0; v < e->fp_colors_len; ++v)
        if (e->loop_fpr[v] >= 0) {
          e->fp_color_seeded[v] =
              nyr_mach_regalloc_live_in(&e->fp_regalloc, bi, v) &&
              (e->loop_fpr_entry[bi] & (1u << e->loop_fpr[v])) != 0;
        }
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
      if (e->fp_last_def_inst && in->dst.kind == NYR_MACH_OPERAND_VREG &&
          in->dst.as.reg < e->fp_colors_len)
        e->fp_last_def_inst[in->dst.as.reg] = e->cur_inst + 1;
      e->aliased_dst_home =
          in->opcode != NYR_MACH_COPY && in->opcode != NYR_MACH_STORE &&
                  in->dst.kind == NYR_MACH_OPERAND_VREG &&
                  mach_inst_input_uses_vreg(in, in->dst.as.reg)
              ? mach_vreg_home_off(e, in->dst.as.reg)
              : 0;
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
          if (e->vec_fast_path && vt == NYR_MACH_TYPE_V128_I64 &&
              !mach_is_v128(mach, &in->src0) &&
              in->src0.kind == NYR_MACH_OPERAND_VREG) {
            int assigned = in->dst.kind == NYR_MACH_OPERAND_VREG
                               ? mach_vreg_vpreg(e, in->dst.as.reg)
                               : -1;
            unsigned d = assigned >= 0 ? (unsigned)assigned : 14;
            if (!mach_load_op_rax(e, &in->src0) || !mach_mov_rax_xmm(e, d) ||
                !mach_vec_op_reg(e, d, d, 0x66, 0x6c))
              return false;
            if (assigned >= 0) {
              if (e->vec_color_seeded)
                e->vec_color_seeded[in->dst.as.reg] = true;
            } else if (!mach_store_xmm_v128(e, dst, d)) {
              return false;
            }
            break;
          }
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
              if (!mach_mov_rax_imm(e, in->src0.as.imm))
                return false;
              if (vt == NYR_MACH_TYPE_V256_F64) {
                if (!mach_store_rax(e, dst) ||
                    !mach_ymm_broadcastsd_mem(e, 0, dst) ||
                    !mach_ymm_store_v256(e, dst, 0))
                  return false;
              } else if (!mach_ymm_set1_i64_avx_rax(e) ||
                         !mach_ymm_store_v256(e, dst, 0)) {
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
               * write, so canonicalize it to its home first — unless AVX2
               * can broadcast straight from that register.
               */
              int set1_dst = in->dst.kind == NYR_MACH_OPERAND_VREG
                                 ? mach_vreg_vpreg(e, in->dst.as.reg)
                                 : -1;
              unsigned set1_reg = set1_dst >= 0 ? (unsigned)set1_dst : 0;
              bool avx2 =
                  e->target && (e->target->caps & NY_NATIVE_CAP_MACH_AVX2);
              if (vt == NYR_MACH_TYPE_V256_F64 && avx2 &&
                  in->src0.kind == NYR_MACH_OPERAND_VREG &&
                  e->mach->vreg_types[in->src0.as.reg] == NYR_MACH_TYPE_F64) {
                int fp = mach_vreg_fpreg(e, in->src0.as.reg);
                if (fp >= 0 && e->fp_color_seeded &&
                    e->fp_color_seeded[in->src0.as.reg]) {
                  if (!mach_ymm_broadcastsd_reg(e, set1_reg, (unsigned)fp))
                    return false;
                  if (set1_dst >= 0) {
                    if (e->vec_color_seeded)
                      e->vec_color_seeded[in->dst.as.reg] = true;
                  } else if (!mach_ymm_store_v256(e, dst, 0)) {
                    return false;
                  }
                  break;
                }
              }
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
                /*
                 * A broadcast load cannot take its value by store-to-load
                 * forwarding, and the scalar home is usually written just
                 * before (a freshly computed multiplier). Load the lane with
                 * movsd, which forwards, and broadcast from the register.
                 */
                if (avx2 && mach_is_float(mach, &in->src0)) {
                  if (!mach_load_xmm(e, a, false, set1_reg) ||
                      !mach_ymm_broadcastsd_reg(e, set1_reg, set1_reg))
                    return false;
                } else if (!mach_ymm_broadcastsd_mem(e, set1_reg, a))
                  return false;
              } else {
                if (!mach_load_op_rax(e, &in->src0) ||
                    !mach_ymm_set1_i64_avx_rax(e) ||
                    (set1_reg != 0 && !mach_ymm_mov_reg(e, set1_reg, 0)))
                  return false;
              }
              if (set1_dst >= 0) {
                if (e->vec_color_seeded)
                  e->vec_color_seeded[in->dst.as.reg] = true;
              } else if (!mach_ymm_store_v256(e, dst, 0)) {
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
               * SET1_F64: vbroadcastsd ymm0, [rbp+a]. The scalar may be
               * resident only in its float color (a constant or a value
               * whose home is not yet written), so read it through the
               * float operand path and broadcast from the destination's low
               * lane instead of the source home.
               */
              unsigned s = 0;
              if (!mach_load_float_operand(e, &in->src0, false, 0, &s) ||
                  !mach_store_xmm(e, dst, false, s) ||
                  !mach_ymm_broadcastsd_mem(e, 0, dst) ||
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
          } else if (mach_is_v128(mach, &in->src0) &&
                     (vt == NYR_MACH_TYPE_V256_I64 ||
                      vt == NYR_MACH_TYPE_V256_F64)) {
            /*
             * Home-to-home copy of a four-lane value outside the vector fast
             * path: a 16-byte movdqu would leave the upper lanes stale.
             */
            if (!mach_ymm_load_v256(e, a, 0) || !mach_ymm_store_v256(e, dst, 0))
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
            if (!mach_load_op_rax(e, &in->src0) ||
                !mach_ymm_set1_i64_avx_rax(e) ||
                !mach_ymm_store_v256(e, dst, 0))
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
          /*
           * The loop-home allocator only gives a latch producer its PHI
           * destination's preg after proving the producer is a one-use
           * value and that the old destination is dead until this edge copy.
           * Both values then have the same dedicated function-wide home, so
           * this copy neither needs a move nor a canonical-home write.
           */
          if (in->src0.as.reg < e->mach->vreg_len &&
              in->dst.as.reg < e->mach->vreg_len &&
              e->color_seeded[in->src0.as.reg] && e->loop_gpr &&
              e->loop_gpr[in->src0.as.reg] >= 0 &&
              e->loop_gpr[in->src0.as.reg] == e->loop_gpr[in->dst.as.reg]) {
            e->color_seeded[in->dst.as.reg] = true;
            break;
          }
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
         * An uncolored literal whose only consumer is the next instruction's
         * load address is rematerialized there from imm_fold; publishing it
         * through %rax first only produced a duplicate `movabs rax, imm`.
         * Other consumers may read the home, so they keep the publish.
         */
        if (in->src0.kind == NYR_MACH_OPERAND_IMM &&
            in->dst.kind == NYR_MACH_OPERAND_VREG && !in->narrow32 &&
            mach_is_i64(mach, &in->dst) && e->decolored_literal &&
            e->imm_fold_known && in->dst.as.reg < mach->vreg_len &&
            e->decolored_literal[in->dst.as.reg] &&
            e->imm_fold_known[in->dst.as.reg] &&
            mach_vreg_def_preg(e, in->dst.as.reg) < 0 &&
            mach_vreg_can_forward_to_next_rax(e, in->dst.as.reg) &&
            e->cur_inst + 1 < mach->inst_len &&
            mach->insts[e->cur_inst + 1].opcode == NYR_MACH_LOAD &&
            mach->insts[e->cur_inst + 1].src0.kind == NYR_MACH_OPERAND_VREG &&
            mach->insts[e->cur_inst + 1].src0.as.reg == in->dst.as.reg &&
            !(mach->insts[e->cur_inst + 1].src1.kind == NYR_MACH_OPERAND_VREG &&
              mach->insts[e->cur_inst + 1].src1.as.reg == in->dst.as.reg)) {
          if (e->last_rax_valid &&
              e->last_rax_off == mach_vreg_home_off(e, in->dst.as.reg))
            mach_rax_invalidate(e);
          break;
        }
        /*
         * An uncolored literal whose only consumer is the next instruction's
         * load address is rematerialized there from imm_fold; publishing it
         * through %rax first only produced a duplicate `movabs rax, imm`.
         * Other consumers may read the home, so they keep the publish.
         */
        if (in->src0.kind == NYR_MACH_OPERAND_IMM &&
            in->dst.kind == NYR_MACH_OPERAND_VREG && !in->narrow32 &&
            mach_is_i64(mach, &in->dst) && e->decolored_literal &&
            e->imm_fold_known && in->dst.as.reg < mach->vreg_len &&
            e->decolored_literal[in->dst.as.reg] &&
            e->imm_fold_known[in->dst.as.reg] &&
            mach_vreg_def_preg(e, in->dst.as.reg) < 0 &&
            mach_vreg_can_forward_to_next_rax(e, in->dst.as.reg) &&
            e->cur_inst + 1 < mach->inst_len &&
            mach->insts[e->cur_inst + 1].opcode == NYR_MACH_LOAD &&
            mach->insts[e->cur_inst + 1].src0.kind == NYR_MACH_OPERAND_VREG &&
            mach->insts[e->cur_inst + 1].src0.as.reg == in->dst.as.reg &&
            !(mach->insts[e->cur_inst + 1].src1.kind == NYR_MACH_OPERAND_VREG &&
              mach->insts[e->cur_inst + 1].src1.as.reg == in->dst.as.reg)) {
          if (e->last_rax_valid &&
              e->last_rax_off == mach_vreg_home_off(e, in->dst.as.reg))
            mach_rax_invalidate(e);
          break;
        }
        /*
         * Constant into a colored destination: materialize it in place
         * (`mov r32/r64, imm`, which leaves flags and RAX intact) instead of
         * staging through RAX and copying.
         */
        if (in->src0.kind == NYR_MACH_OPERAND_IMM &&
            in->dst.kind == NYR_MACH_OPERAND_VREG &&
            mach_is_i64(mach, &in->dst) && e->color_seeded &&
            in->dst.as.reg < e->colors_len && !in->narrow32) {
          int dp = mach_vreg_def_preg(e, in->dst.as.reg);
          if (dp > 0 && dp != 4 && dp != 5) {
            if (!mach_mov_preg_imm(e, (unsigned)dp, in->src0.as.imm))
              return false;
            for (uint32_t v = 0; v < e->colors_len; ++v) {
              if (v == in->dst.as.reg || !e->color_seeded[v])
                continue;
              if (mach_vreg_preg(e, v) == dp)
                e->color_seeded[v] = false;
            }
            if (mach_vreg_requires_home_after_def(e, in->dst.as.reg) &&
                !mach_store_preg_home(e, dp,
                                      mach_vreg_home_off(e, in->dst.as.reg)))
              return false;
            if (dp == 10)
              mach_r10_index_invalidate(e);
            if (e->last_rax_valid &&
                e->last_rax_off == mach_vreg_home_off(e, in->dst.as.reg))
              mach_rax_invalidate(e);
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
          int saved_rax_vreg = e->last_rax_vreg;
          if (!mach_seed_colored_preg(e, in->src0.as.reg))
            return false;
          int sp = mach_vreg_preg(e, in->src0.as.reg);
          int dp = mach_vreg_def_preg(e, in->dst.as.reg);
          if (sp >= 0 && dp >= 0 && (sp != dp || e->same_color_copy_fold)) {
            /*
             * A copy whose source and destination already share a color
             * needs no move at all. Staging it through %rax produced
             * `mov %rax,%r13; mov %r13,%rax` pairs around every coalesced
             * definition (37 adjacent pairs in `knight`'s `moves`).
             */
            if (sp != dp && !mach_mov_preg_preg(e, dp, sp))
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
            /*
             * Seeding and copying between non-RAX registers preserve RAX.
             * Retain its cached home only if this definition cannot replace
             * the value stored at that same home. Segment-end stores still
             * invalidate the cache through their existing path.
             */
            if (saved_rax_valid && dp != 0 && sp != 0 &&
                saved_rax_off != mach_vreg_home_off(e, in->dst.as.reg)) {
              e->last_rax_valid = true;
              e->last_rax_off = saved_rax_off;
              e->last_rax_vreg = saved_rax_vreg;
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
        bool fused = false;
        if (!mach_try_fuse_load_add(e, in, &fused))
          return false;
        if (fused) {
          ++n;
          break;
        }
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
            bool can_fold_rhs = vt == NYR_MACH_TYPE_V256_F64 ||
                                (vt == NYR_MACH_TYPE_V256_I64 && e->target &&
                                 (e->target->caps & NY_NATIVE_CAP_MACH_AVX2));
            if (can_fold_rhs && e->cur_inst + 1 < mach->inst_len &&
                mach_vector_ymm_rhs_load(e, e->cur_inst, e->cur_inst + 1,
                                         &mach->insts[e->cur_inst + 1].src1,
                                         vt))
              break;
            if (!mach_vector_ymm_load(e, in, dst))
              return false;
            break;
          }
          if (vt == NYR_MACH_TYPE_V256_I64 || vt == NYR_MACH_TYPE_V256_F64) {
            /*
             * Without the vector fast path there is no resident YMM color, but
             * the address still has to honor a pending SIB fold left by the
             * adjacent ADD.  The old hand-rolled fallback reloaded the address
             * vreg's stale home through RAX, dereferencing whatever constant
             * (STREAM's N) happened to occupy that slot.  mach_vector_ymm_load
             * performs the SIB query and already falls back to the same RAX
             * path when the address was not folded.
             */
            if (!mach_vector_ymm_load(e, in, dst))
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
            if (!mach_sse_mem_prefix(e, f32, &A, reg, 0x10) ||
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
          } else if (a == dst && incoming < 0) {
            /*
             * The parameter slot is this value's home: nothing to copy.
             */
          } else if (!mach_load_rax(e, a) ||
                     !mach_commit_vreg(e, &in->dst, dst)) {
            return false;
          }
        } else {
          ny_x64_mem_addr_t A;
          if (!mach_resolve_mem_addr(e, &in->src0, &A))
            return false;
          /*
           * Load straight into an allocated destination; going through RAX
           * would only add a copy.
           */
          int load_preg = in->dst.kind == NYR_MACH_OPERAND_VREG
                              ? mach_vreg_def_preg(e, in->dst.as.reg)
                              : -1;
          unsigned load_reg = load_preg >= 0 ? (unsigned)load_preg : 0u;
          bool load_r = load_reg >= 8;
          if (in->sext_load && in->byte_width && in->mem_size != 4 &&
              in->mem_size != 5) {
            /*
             * movsx r64, byte/word ptr: one instruction for the load and
             * its sign extension.
             */
            if (!mach_rex_for_addr(e, &A, true, load_r) || !mach_u8(e, 0x0f) ||
                !mach_u8(e, in->mem_size == 2 ? 0xbf : 0xbe) ||
                !mach_emit_mem(e, &A, load_reg & 7))
              return false;
          } else if (in->mem_size == 5) {
            /*
             * mov r32, dword ptr: the 32-bit write clears the high half.
             */
            if (!mach_rex_for_addr(e, &A, false, load_r) || !mach_u8(e, 0x8b) ||
                !mach_emit_mem(e, &A, load_reg & 7))
              return false;
          } else if (in->mem_size == 2) {
            /*
             * movzx r32, word ptr: the 32-bit write clears the high half.
             */
            if (!mach_rex_for_addr(e, &A, false, load_r) || !mach_u8(e, 0x0f) ||
                !mach_u8(e, 0xb7) || !mach_emit_mem(e, &A, load_reg & 7))
              return false;
          } else if (in->mem_size == 4) {
            /*
             * movsxd r64, dword ptr
             */
            if (!mach_rex_for_addr(e, &A, true, load_r) || !mach_u8(e, 0x63) ||
                !mach_emit_mem(e, &A, load_reg & 7))
              return false;
          } else if (in->byte_width) {
            if (!mach_rex_for_addr(e, &A, false, load_r) || !mach_u8(e, 0x0f) ||
                !mach_u8(e, 0xb6) || !mach_emit_mem(e, &A, load_reg & 7))
              return false;
          } else if (!mach_rex_for_addr(e, &A, true, load_r) ||
                     !mach_u8(e, 0x8b) || !mach_emit_mem(e, &A, load_reg & 7)) {
            return false;
          }
          /*
           * The dereference replaced the address in RAX with its payload.
           * A reused destination home can equal the address's old home;
           * retaining that sticky tag would incorrectly omit the store.
           */
          mach_rax_invalidate(e);
          if (load_preg >= 0) {
            if (!mach_commit_preg(e, &in->dst, dst, load_reg, false))
              return false;
          } else if (!mach_commit_vreg(e, &in->dst, dst)) {
            return false;
          }
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
                  !mach_sse_mem_prefix(e, f32, &A, reg, 0x11) ||
                  !mach_emit_mem(e, &A, reg & 7))
                return false;
            } else if (!mach_load_xmm0(e, a, f32) ||
                       !mach_sse_mem_prefix(e, f32, &A, 0, 0x11) ||
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
            if (in->mem_size == 2) {
              if (!mach_u8(e, 0x66) ||
                  !mach_rex_for_addr(e, &A, false, false) ||
                  !mach_u8(e, 0xc7) || !mach_emit_mem(e, &A, 0) ||
                  !mach_u8(e, (unsigned char)(simm & 0xff)) ||
                  !mach_u8(e, (unsigned char)((simm >> 8) & 0xff)))
                return false;
            } else if (in->mem_size == 4) {
              if (!mach_rex_for_addr(e, &A, false, false) ||
                  !mach_u8(e, 0xc7) || !mach_emit_mem(e, &A, 0) ||
                  !mach_i32(e, (int32_t)simm))
                return false;
            } else if (in->byte_width) {
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
            if (in->mem_size == 2) {
              if (!mach_u8(e, 0x66) ||
                  !mach_rex_for_addr(e, &A, false, src_preg >= 8) ||
                  !mach_u8(e, 0x89) || !mach_emit_mem(e, &A, src_preg & 7))
                return false;
            } else if (in->mem_size == 4) {
              if (!mach_rex_for_addr(e, &A, false, src_preg >= 8) ||
                  !mach_u8(e, 0x89) || !mach_emit_mem(e, &A, src_preg & 7))
                return false;
            } else if (in->byte_width) {
              /*
               * Without a REX prefix, byte registers 4-7 are ah/ch/dh/bh, not
               * spl/bpl/sil/dil, so those sources need an empty one.
               */
              bool plain_rex = src_preg >= 4 && src_preg < 8 &&
                               (unsigned)A.base < 8 &&
                               !(A.index >= 0 && (unsigned)A.index >= 8);
              if ((plain_rex && !mach_u8(e, 0x40)) ||
                  !mach_rex_for_addr(e, &A, false, src_preg >= 8) ||
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
         * YMM loads in the vector fast path fold indexed LEAs through
         * mach_sib_query. Other vector load paths still need the pointer
         * materialized in its home.
         */
        const nyr_mach_inst_t *next = e->cur_inst + 1 < mach->inst_len
                                          ? &mach->insts[e->cur_inst + 1]
                                          : NULL;
        bool next_is_vector_load =
            next && next->opcode == NYR_MACH_LOAD &&
            mach_is_v128(mach, &next->dst) &&
            !(e->vec_fast_path &&
              (mach_v128_type(mach, &next->dst) == NYR_MACH_TYPE_V256_F64 ||
               mach_v128_type(mach, &next->dst) == NYR_MACH_TYPE_V256_I64));
        /*
         * Scalar-float lowering may insert conversions between an address
         * LEA and its memory use. The SIB rematerializer only consumes an
         * adjacent use, so eliding this LEA would leave its home uninitialized.
         *
         * An adjacent float access folds this LEA whenever both inputs are
         * resident, so the address need not be materialized for it. That
         * holds for a float store as much as for a load: mach_sib_query
         * decides both from the same residency of base and index.
         */
        bool float_use_folds =
            e->fp_fast_path && !e->force_stack_reads && next &&
            (next->opcode == NYR_MACH_LOAD ||
             (next->opcode == NYR_MACH_STORE &&
              mach_is_float(mach, &next->src0))) &&
            in->dst.kind == NYR_MACH_OPERAND_VREG &&
            in->src0.kind == NYR_MACH_OPERAND_VREG &&
            in->src1.kind == NYR_MACH_OPERAND_VREG &&
            mach_sib_lea_mem(e, in->dst.as.reg, e->cur_inst + 1, NULL, NULL,
                             NULL, NULL);
        if (float_use_folds) {
          /*
           * Residency is asked where the load will read it: an input whose
           * last use is this LEA has already left its register there.
           */
          size_t lea_inst = e->cur_inst++;
          int base_at_load = mach_vreg_preg(e, in->src0.as.reg);
          int index_at_load = mach_vreg_preg(e, in->src1.as.reg);
          e->cur_inst = lea_inst;
          /*
           * R12 is a valid SIB index (REX.X); only RSP cannot be one. This
           * matches the test mach_sib_query applies at the access.
           */
          float_use_folds =
              base_at_load >= 0 && index_at_load >= 0 && index_at_load != 4;
        }
        if (in->dst.kind == NYR_MACH_OPERAND_VREG && !next_is_vector_load &&
            (float_use_folds || !mach_sib_lea_has_float_memory_use(
                                    e, in->dst.as.reg, e->cur_inst)) &&
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
            /*
             * A literal base is a displacement: `lea dst, [index*s + imm]`
             * (SIB with no base) instead of materializing it in RAX first.
             */
            if (base_preg < 0 && index != 10 && e->imm_fold &&
                e->imm_fold_known && in->src0.kind == NYR_MACH_OPERAND_VREG &&
                in->src0.as.reg < mach->vreg_len &&
                e->imm_fold_known[in->src0.as.reg] &&
                e->imm_fold[in->src0.as.reg] >= INT32_MIN &&
                e->imm_fold[in->src0.as.reg] <= INT32_MAX) {
              int dst_lit = mach_vreg_def_preg(e, in->dst.as.reg);
              unsigned dreg = dst_lit >= 0 ? (unsigned)dst_lit : 0;
              if (!mach_rex(e, true, dreg >= 8, index >= 8, false) ||
                  !mach_u8(e, 0x8d) ||
                  !mach_u8(e, (unsigned char)(((dreg & 7) << 3) | 0x04)) ||
                  !mach_u8(e, (unsigned char)((scale << 6) |
                                              ((index & 7) << 3) | 5)) ||
                  !mach_i32(e, (int32_t)e->imm_fold[in->src0.as.reg]))
                return false;
              if (dst_lit >= 0) {
                if (!mach_commit_preg(e, &in->dst, dst, dreg, false))
                  return false;
              } else {
                mach_rax_invalidate(e);
                if (!mach_commit_vreg(e, &in->dst, dst))
                  return false;
              }
              break;
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
              !mach_rbp_disp(e, 0, a) || !mach_commit_vreg(e, &in->dst, dst))
            return false;
        } else if (in->src0.kind == NYR_MACH_OPERAND_SYMBOL &&
                   in->src0.as.symbol) {
          /*
           * leaq sym(%rip), reg — 48 8d 05 rel32 with reg in ModRM.
           * Materializing the address in RAX and moving it to the assigned
           * color cost ten bytes per address-of-global use; the move is the
           * largest single redundancy left in the encoder's output on
           * fixtures that pass globals by address.
           */
          int dst_preg = in->dst.kind == NYR_MACH_OPERAND_VREG
                             ? mach_vreg_def_preg(e, in->dst.as.reg)
                             : -1;
          if (e->lea_assign_color && dst_preg >= 0 && dst_preg != 4 &&
              dst_preg != 5) {
            /*
             * The same commit as the indexed LEA paths above: the assigned
             * register now holds the address, and a later consumer that
             * re-seeds from the canonical home must still find it current.
             */
            ++e->stat_lea_color;
            if (!mach_remat_addr_preg(e, dst_preg, &in->src0) ||
                !mach_commit_preg(e, &in->dst, dst, (unsigned)dst_preg, false))
              return false;
          } else {
            ++e->stat_lea_staged;
            if (!mach_rex(e, true, false, false, false) || !mach_u8(e, 0x8d) ||
                !mach_u8(e, 0x05) ||
                !mach_add_reloc(e, in->src0.as.symbol, NY_RELOC_PC32) ||
                !mach_i32(e, 0) || !mach_commit_vreg(e, &in->dst, dst))
              return false;
          }
        } else
          return mach_err(e, "x64 machine form encode: bad LEA");
        break;
      }
      case NYR_MACH_CONVERT:
        if (mach_v128_type(mach, &in->dst) == NYR_MACH_TYPE_V128_F64 &&
            mach_v128_type(mach, &in->src0) == NYR_MACH_TYPE_V128_I64) {
          /*
           * SSE2 has no signed qword-to-double packed conversion. Convert
           * both full-width signed lanes separately, then pack them. This
           * retains CVTSI2SD rounding even beyond the exact-double range.
           * xmm14/xmm15 and rax are encoder scratch registers.
           */
          unsigned input_reg = 14;
          if (!mach_load_vector_operand(e, &in->src0, 14, &input_reg) ||
              !mach_vec_mov_reg(e, 15, input_reg) ||
              /*
               * psrldq xmm15, 8
               */
              !mach_u8(e, 0x66) || !mach_rex(e, false, false, false, true) ||
              !mach_u8(e, 0x0f) || !mach_u8(e, 0x73) || !mach_u8(e, 0xdf) ||
              !mach_u8(e, 8) || !mach_mov_xmm_rax(e, input_reg) ||
              /*
               * cvtsi2sd xmm14, rax
               */
              !mach_u8(e, 0xf2) || !mach_rex(e, true, true, false, false) ||
              !mach_u8(e, 0x0f) || !mach_u8(e, 0x2a) || !mach_u8(e, 0xf0) ||
              !mach_mov_xmm_rax(e, 15) ||
              /*
               * cvtsi2sd xmm15, rax
               */
              !mach_u8(e, 0xf2) || !mach_rex(e, true, true, false, false) ||
              !mach_u8(e, 0x0f) || !mach_u8(e, 0x2a) || !mach_u8(e, 0xf8) ||
              !mach_vec_op_reg(e, 14, 15, 0x66, 0x14))
            return false;
          mach_rax_invalidate(e);
          int assigned = in->dst.kind == NYR_MACH_OPERAND_VREG
                             ? mach_vreg_vpreg(e, in->dst.as.reg)
                             : -1;
          if (assigned >= 0) {
            if (!mach_vec_mov_reg(e, (unsigned)assigned, 14))
              return false;
            if (e->vec_color_seeded)
              e->vec_color_seeded[in->dst.as.reg] = true;
          } else if (!mach_store_xmm_v128(e, dst, 14)) {
            return false;
          }
          break;
        }
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
          /*
           * cvttsd2si rax, src. The source stays where it is: staging it
           * through xmm0 would destroy any float allocated there.
           */
          if (!mach_u8(e, f32 ? 0xf3 : 0xf2) ||
              !mach_rex(e, true, false, false, src_reg >= 8) ||
              !mach_u8(e, 0x0f) || !mach_u8(e, 0x2c) ||
              !mach_u8(e, (unsigned char)(0xc0 | (src_reg & 7))))
            return false;
          mach_rax_invalidate(e);
          if (!mach_commit_vreg(e, &in->dst, dst))
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
        if (!mach_u8(e, 0xdd) || !mach_rbp_disp(e, 0, a) || !mach_u8(e, 0xd9) ||
            !mach_u8(e, in->opcode == NYR_MACH_SIN ? 0xfe : 0xff) ||
            !mach_u8(e, 0xdd) || !mach_rbp_disp(e, 3, dst))
          return false;
        break;
      case NYR_MACH_POPCOUNT64:
        if (!mach_is_i64(mach, &in->dst) || !mach_is_i64(mach, &in->src0))
          return mach_err(
              e, "x64 machine form encode: popcount needs i64 operands");
        {
          /*
           * popcnt dst, src (F3 REX.W 0F B8 /r) straight between the
           * allocated registers; otherwise through RAX with the ordinary
           * commit (register, forwarded RAX, or home) instead of an
           * unconditional home store.
           */
          int pd = in->dst.kind == NYR_MACH_OPERAND_VREG
                       ? mach_vreg_def_preg(e, in->dst.as.reg)
                       : -1;
          int ps = -1;
          if (pd >= 0 && in->src0.kind == NYR_MACH_OPERAND_VREG &&
              mach_seed_colored_preg(e, in->src0.as.reg))
            ps = mach_vreg_preg(e, in->src0.as.reg);
          if (pd >= 0 && ps >= 0) {
            if (!mach_u8(e, 0xf3) ||
                !mach_rex(e, true, pd >= 8, false, ps >= 8) ||
                !mach_u8(e, 0x0f) || !mach_u8(e, 0xb8) ||
                !mach_u8(e,
                         (unsigned char)(0xc0 | ((pd & 7) << 3) | (ps & 7))) ||
                !mach_commit_preg(e, &in->dst, dst, (unsigned)pd, false))
              return false;
            break;
          }
          if (!mach_load_op_rax(e, &in->src0))
            return false;
          if (!mach_u8(e, 0xf3) || !mach_rex(e, true, false, false, false) ||
              !mach_u8(e, 0x0f) || !mach_u8(e, 0xb8) || !mach_u8(e, 0xc0))
            return false;
          mach_rax_invalidate(e);
          if (!mach_commit_vreg(e, &in->dst, dst))
            return false;
        }
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
      case NYR_MACH_SHR:
      case NYR_MACH_VSHR:
        if (in->opcode == NYR_MACH_SHL &&
            in->dst.kind == NYR_MACH_OPERAND_VREG &&
            mach_sib_scaled_index_only_feeds_add(e, in->dst.as.reg))
          break;
        if (in->opcode == NYR_MACH_SHL && mach_shl_add_fuse_begin(e, in))
          break;
        if (in->opcode == NYR_MACH_ADD && e->shl_fuse_valid &&
            e->shl_fuse_add == e->cur_inst) {
          if (!mach_shl_add_fuse_emit(e, in, dst))
            return false;
          break;
        }
        if ((in->opcode == NYR_MACH_AND || in->opcode == NYR_MACH_SUB) &&
            in->dst.kind == NYR_MACH_OPERAND_VREG && e->select_skip_alu &&
            in->dst.as.reg < e->colors_len &&
            e->select_skip_alu[in->dst.as.reg])
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
              /*
               * Consume loop-resident address operands directly in SIB.
               */
              e->pending_sib_base = (unsigned)bp;
              e->pending_sib_index = (unsigned)ip;
            } else if (ip >= 0 && ip != 4 && (unsigned)ip != 10u) {
              /*
               * Only the base needs a scratch: keep the resident index.
               */
              if (!mach_seed_colored_preg(e, index->as.reg) ||
                  !mach_load_op_rax(e, base))
                return false;
              e->pending_sib_base = 0;
              e->pending_sib_index = (unsigned)ip;
            } else if (bp >= 0 && bp != 4) {
              /*
               * Only the index needs a scratch: keep the resident base.
               */
              if (!mach_seed_colored_preg(e, base->as.reg) ||
                  !mach_load_op_preg(e, index, 10))
                return false;
              e->pending_sib_base = (unsigned)bp;
              e->pending_sib_index = 10;
            } else {
              /*
               * Otherwise materialize the address pair in reserved scratches.
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
          /*
           * Packed i64 arithmetic shift has its own exact AVX2 sequence.
           */
          if (in->opcode == NYR_MACH_SAR && vt == NYR_MACH_TYPE_V256_I64) {
            if (!mach_vector_ymm_sar(e, in, dst))
              return false;
            break;
          }
          if (in->opcode == NYR_MACH_VSHR && vt == NYR_MACH_TYPE_V256_I64) {
            if (!mach_vector_ymm_shr(e, in, dst))
              return false;
            break;
          }
          if (in->opcode == NYR_MACH_MUL && vt == NYR_MACH_TYPE_V256_I64) {
            if (!mach_vector_ymm_mul_i64(e, in, dst))
              return false;
            break;
          }
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
              (vt == NYR_MACH_TYPE_V128_I64 || vt == NYR_MACH_TYPE_V128_F64 ||
               vt == NYR_MACH_TYPE_V128_F32)) {
            unsigned prefix =
                vt == NYR_MACH_TYPE_V128_I64 || vt == NYR_MACH_TYPE_V128_F64
                    ? 0x66
                    : 0;
            unsigned opc = vt == NYR_MACH_TYPE_V128_I64
                               ? in->opcode == NYR_MACH_ADD   ? 0xd4
                                 : in->opcode == NYR_MACH_SUB ? 0xfb
                                 : in->opcode == NYR_MACH_AND ? 0xdb
                                 : in->opcode == NYR_MACH_OR  ? 0xeb
                                 : in->opcode == NYR_MACH_XOR ? 0xef
                                                              : 0xff
                           : in->opcode == NYR_MACH_ADD ? 0x58
                           : in->opcode == NYR_MACH_SUB ? 0x5c
                           : in->opcode == NYR_MACH_MUL ? 0x59
                           : in->opcode == NYR_MACH_DIV ? 0x5e
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
          if (vt == NYR_MACH_TYPE_V256_I64 &&
              (in->opcode == NYR_MACH_ADD || in->opcode == NYR_MACH_SUB ||
               in->opcode == NYR_MACH_AND || in->opcode == NYR_MACH_OR ||
               in->opcode == NYR_MACH_XOR)) {
            unsigned char opc = in->opcode == NYR_MACH_ADD   ? 0xd4
                                : in->opcode == NYR_MACH_SUB ? 0xfb
                                : in->opcode == NYR_MACH_AND ? 0x54
                                : in->opcode == NYR_MACH_OR  ? 0x56
                                                             : 0x57;
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
             * vpaddq/vpsubq/vpand/vpor/vpxor ymm — VEX.256.66.0F.W1 opc.
             * Only those five opcodes have a packed form here; anything else
             * reaching this legacy path would be miscompiled as XOR.
             */
            if (in->opcode != NYR_MACH_ADD && in->opcode != NYR_MACH_SUB &&
                in->opcode != NYR_MACH_AND && in->opcode != NYR_MACH_OR &&
                in->opcode != NYR_MACH_XOR) {
              char msg[128];
              snprintf(msg, sizeof(msg),
                       "x64 machine form encode: no packed i64 form for %s",
                       nyr_mach_opcode_name(in->opcode));
              return mach_err(e, msg);
            }
            unsigned char opc = in->opcode == NYR_MACH_ADD   ? 0xd4
                                : in->opcode == NYR_MACH_SUB ? 0xfb
                                : in->opcode == NYR_MACH_AND ? 0xdb
                                : in->opcode == NYR_MACH_OR  ? 0xeb
                                                             : 0xef;
            if (!mach_u8(e, 0xc4) || !mach_u8(e, 0xe1) || !mach_u8(e, 0x7d) ||
                !mach_u8(e, opc) || !mach_rbp_disp(e, 0, b) ||
                !mach_u8(e, 0xc4) || !mach_u8(e, 0xe1) || !mach_u8(e, 0x7d) ||
                !mach_u8(e, 0x7f) || !mach_rbp_disp(e, 0, dst))
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
                !mach_u8(e, 0x6f) || !mach_rbp_disp(e, 0, a) ||
                !mach_u8(e, 0xc4) || !mach_u8(e, 0xe1) || !mach_u8(e, 0x7d) ||
                !mach_u8(e, opc) || !mach_rbp_disp(e, 0, b) ||
                !mach_u8(e, 0xc4) || !mach_u8(e, 0xe1) || !mach_u8(e, 0x7e) ||
                !mach_u8(e, 0x7f) || !mach_rbp_disp(e, 0, dst))
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
            in->opcode == NYR_MACH_SHR || in->opcode == NYR_MACH_ROR ||
            in->opcode == NYR_MACH_ROR32) {
          if (in->opcode == NYR_MACH_SAR &&
              mach_v128_type(mach, &in->dst) == NYR_MACH_TYPE_V256_I64) {
            if (!mach_vector_ymm_sar(e, in, dst))
              return false;
            break;
          }
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
                        : in->opcode == NYR_MACH_SHR ? 0xe8
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
          if (fold_ok &&
              (in->opcode == NYR_MACH_SHL || in->opcode == NYR_MACH_SAR ||
               in->opcode == NYR_MACH_SHR || in->opcode == NYR_MACH_ROR ||
               in->opcode == NYR_MACH_ROR32) &&
              in->dst.kind == NYR_MACH_OPERAND_VREG &&
              in->src0.kind == NYR_MACH_OPERAND_VREG) {
            /*
             * MIR shifts already have explicit bitwise semantics: tagged
             * language operations have been legalized before this point.
             * Destination-preg immediate shift: stage the current value
             * straight into the destination register and shift there,
             * skipping the %rax round trip. ROR32 is a real 32-bit rotate,
             * whose result the hardware zero-extends.
             */
            int dp = mach_vreg_def_preg(e, in->dst.as.reg);
            if (dp >= 0) {
              if (!mach_load_src_preg(e, &in->src0, (unsigned)dp))
                return false;
              if (in->sext32) {
                if (!mach_rex(e, true, dp >= 8, false, dp >= 8) ||
                    !mach_u8(e, 0x63) ||
                    !mach_u8(e,
                             (unsigned char)(0xc0 | (dp & 7) << 3 | (dp & 7))))
                  return false;
              } else if (is_ror32) {
                unsigned count = (unsigned)(fold & 31);
                if (!mach_rex(e, false, false, false, dp >= 8) ||
                    !mach_u8(e, 0xc1) ||
                    !mach_u8(e, (unsigned char)(0xc8 | (dp & 7))) ||
                    !mach_u8(e, (unsigned char)count))
                  return false;
              } else if (!mach_shift_preg_imm(e, in->opcode, dp, fold & 63)) {
                return false;
              }
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
            if (in->sext32) {
              if (!mach_load_op_rax(e, &in->src0) || !mach_u8(e, 0x48) ||
                  !mach_u8(e, 0x63) || !mach_u8(e, 0xc0))
                return false;
            } else if (!mach_load_op_rax(e, &in->src0) ||
                       (!is_ror32 && !mach_rex(e, true, false, false, false)) ||
                       !mach_u8(e, 0xc1) || !mach_u8(e, op) ||
                       !mach_u8(e, (unsigned char)(fold & 0xff)) ||
                       (is_ror32 && fold == 0 &&
                        (!mach_u8(e, 0x89) || !mach_u8(e, 0xc0))))
              return false;
          } else {
            /*
             * Stage the shifted value before the count: the allocator may
             * home a value that dies at this shift in RCX, and writing the
             * count into CL first would overwrite it. mach_load_op_preg
             * never touches RAX, so the staged value survives.
             */
            if (!mach_load_op_rax(e, &in->src0) ||
                !mach_load_op_preg(e, &in->src1, 1) ||
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
          bool and_mask32 = in->opcode == NYR_MACH_AND &&
                            in->src1.kind == NYR_MACH_OPERAND_VREG &&
                            imm_fold_known &&
                            in->src1.as.reg < mach->vreg_len &&
                            imm_fold_known[in->src1.as.reg] &&
                            imm_fold[in->src1.as.reg] == INT64_C(0xffffffff);
          if (dp >= 0 && ap >= 0 && !and_mask32) {
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
                /*
                 * lea dst,[src+imm] is the three-operand form of a wrapping
                 * sum. Encode the SIB explicitly for RSP/R12 bases.
                 */
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
                /*
                 * A three-register sum needs no copy before the ADD.
                 * Machine arithmetic has no implicit flag consumer; compare
                 * fusion establishes its own flags at the later CMP.
                 */
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
              } else {
                /*
                 * A spilled RHS normally reads its canonical home. If it is
                 * the immediately preceding load, RAX still holds that exact
                 * value (the load forwarding proof suppressed the home write),
                 * so fold it in as a register operand instead of reloading.
                 */
                if (!mach_mov_preg_preg(e, dp, ap))
                  return false;
                int rhs_home = mach_slot_off(e, &in->src1);
                if (dp != 0 && mach_rax_tag_hit(e, rhs_home,
                                               (int)in->src1.as.reg)) {
                  if (!mach_alu_preg_preg(e, in->opcode, dp, 0))
                    return false;
                } else if (!mach_alu_preg_home(e, in->opcode, dp, rhs_home)) {
                  return false;
                }
                emitted = true;
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
          /*
           * Loading the left operand into the destination would destroy a
           * right operand already resident there.
           */
          bool rhs_in_dst =
              dp >= 0 && in->src1.kind == NYR_MACH_OPERAND_VREG &&
              mach_vreg_assigned_preg(e, in->src1.as.reg) == dp &&
              !(imm_fold_known && in->src1.as.reg < mach->vreg_len &&
                imm_fold_known[in->src1.as.reg]);
          bool commutative =
              in->opcode == NYR_MACH_ADD || in->opcode == NYR_MACH_AND ||
              in->opcode == NYR_MACH_OR || in->opcode == NYR_MACH_XOR ||
              in->opcode == NYR_MACH_MUL;
          if (dp >= 0 && (ap < 0 || and_mask32) &&
              (!rhs_in_dst || commutative)) {
            /*
             * Uncolored source: pull its canonical home straight into the
             * destination preg and apply the operation there.  This skips
             * the %rax stage and the def-side publish for chain-heavy
             * integer code (hash rounds, PRNG mixing, PRNG-like loops).
             */
            bool emitted2 = false;
            if (rhs_in_dst) {
              if (!mach_seed_colored_preg(e, in->src1.as.reg) ||
                  !mach_load_op_rax(e, &in->src0))
                return false;
              emitted2 = mach_alu_preg_preg(e, in->opcode, dp, 0);
            if (emitted2 && !e->home_store_counts)
              e->alu_commute_ops++;
            } else if (and_mask32 && ap >= 0) {
              /*
               * The source already lives in a register: one `mov dp32, ap32`
               * both copies it and clears the high half, so the separate
               * 64-bit copy and the in-place zero-extension below are not
               * needed.
               */
              emitted2 = mach_rex(e, false, ap >= 8, false, dp >= 8) &&
                         mach_u8(e, 0x89) &&
                         mach_u8(e, (unsigned char)(0xc0 | ((ap & 7) << 3) |
                                                    (dp & 7)));
              if (!emitted2)
                return false;
            } else if (!mach_load_src_preg(e, &in->src0, (unsigned)dp)) {
              return false;
            }
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
            if (rhs_in_dst) {
              /*
               * The operation was emitted above.
               */
            } else if (and_mask32 && emitted2) {
              /*
               * already emitted as a single mov r32, r32 above
               */
            } else if (and_mask32) {
              /*
               * mov r32,r32 clears the high half without a mask register.
               */
              emitted2 =
                  mach_rex(e, false, dp >= 8, false, dp >= 8) &&
                  mach_u8(e, 0x89) &&
                  mach_u8(e, (unsigned char)(0xc0 | (dp & 7) << 3 | (dp & 7)));
            } else if (fold_ok) {
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
          if (in->src1.kind == NYR_MACH_OPERAND_IMM) {
            fold = in->src1.as.imm;
            fold_ok = (int64_t)(int32_t)fold == fold;
          } else if (in->src1.kind == NYR_MACH_OPERAND_VREG && imm_fold_known &&
                     in->src1.as.reg < mach->vreg_len &&
                     imm_fold_known[in->src1.as.reg]) {
            fold = imm_fold[in->src1.as.reg];
            fold_ok = (int64_t)(int32_t)fold == fold;
          }
          if (in->opcode == NYR_MACH_AND && fold == UINT32_MAX) {
            /*
             * The low-word mask is a zero extension even when the result
             * has no assigned register. Avoid loading a mask from its home.
             */
            if (!mach_u8(e, 0x89) || !mach_u8(e, 0xc0) ||
                !mach_commit_vreg(e, &in->dst, dst))
              return false;
            break;
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
              bool small = fold >= -128 && fold <= 127;
              unsigned ext = in->opcode == NYR_MACH_ADD   ? 0
                             : in->opcode == NYR_MACH_OR  ? 1
                             : in->opcode == NYR_MACH_AND ? 4
                             : in->opcode == NYR_MACH_SUB ? 5
                                                          : 6;
              if (!mach_rex(e, true, false, false, false) ||
                  !mach_u8(e, small ? 0x83 : op) ||
                  (small && !mach_u8(e, 0xc0 | (ext << 3))) ||
                  !(small ? mach_u8(e, (unsigned char)fold)
                          : mach_i32(e, (int32_t)fold)) ||
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
                !mach_u8(e, 0xaf) || !mach_rbp_disp(e, 0, b) ||
                !mach_commit_vreg(e, &in->dst, dst))
              return false;
            break;
          }
          if (!mach_rex(e, true, false, false, false) || !mach_u8(e, op) ||
              !mach_rbp_disp(e, 0, b) || !mach_commit_vreg(e, &in->dst, dst))
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
        if (in->unsigned_arith) {
          /*
           * R10 holds the divisor; xor edx,edx; div r10.
           */
          if (!mach_load_op_rax(e, &in->src1) || !mach_mov_reg_rax(e, 10) ||
              !mach_load_op_rax(e, &in->src0) ||
              !mach_flush_pregs_to_home(e, (1u << 0) | (1u << 1) | (1u << 2) |
                                               (1u << 8) | (1u << 9) |
                                               (1u << 10)) ||
              !mach_u8(e, 0x31) || !mach_u8(e, 0xd2) ||
              !mach_rex(e, true, false, false, true) || !mach_u8(e, 0xf7) ||
              !mach_u8(e, 0xf2))
            return false;
          if (in->opcode == NYR_MACH_MOD &&
              (!mach_rex(e, true, false, false, false) || !mach_u8(e, 0x89) ||
               !mach_u8(e, 0xd0)))
            return false;
          if (!mach_commit_vreg(e, &in->dst, dst))
            return false;
          break;
        }
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
           * RAX and RDX are scratch for DIV; MOD also uses RCX to form the
           * signed remainder. RAX is not allocatable, but publishing its
           * tracked home keeps the encoder's scratch-state invariant.
           *
           * DIV forms its truncation bias in R11, which no loop home uses,
           * so RDX stays allocatable across signed power-of-two division.
           * MOD keeps its RCX/RDX remainder sequence.
           */
          unsigned pow2_clobbers = in->opcode == NYR_MACH_DIV
                                       ? (1u << 0) | (1u << 11)
                                       : (1u << 0) | (1u << 1) | (1u << 2);
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
            int quotient_preg = in->dst.kind == NYR_MACH_OPERAND_VREG
                                    ? mach_vreg_def_preg(e, in->dst.as.reg)
                                    : -1;
            if (pow2_shift == 1 && direct_dividend_preg >= 0 &&
                quotient_preg > 0 && quotient_preg != direct_dividend_preg) {
              /*
               * q = ((unsigned)x >> 63) + x; q >>= 1. Preserve x for
               * subsequent uses and compute directly in the result home.
               */
              if (!mach_mov_preg_preg(e, quotient_preg, direct_dividend_preg) ||
                  !mach_rex(e, true, false, false, quotient_preg >= 8) ||
                  !mach_u8(e, 0xc1) ||
                  !mach_u8(e, (unsigned char)(0xe8 | (quotient_preg & 7))) ||
                  !mach_u8(e, 63) ||
                  !mach_rex(e, true, direct_dividend_preg >= 8, false,
                            quotient_preg >= 8) ||
                  !mach_u8(e, 0x01) ||
                  !mach_u8(e,
                           (unsigned char)(0xc0 |
                                           ((direct_dividend_preg & 7) << 3) |
                                           (quotient_preg & 7))) ||
                  !mach_shift_preg_imm(e, NYR_MACH_SAR, quotient_preg, 1) ||
                  !mach_commit_preg(e, &in->dst, dst, (unsigned)quotient_preg,
                                    false))
                return false;
              break;
            }
            if (direct_dividend_preg >= 0) {
              /*
               * q = (x + ((x >> 63) & (2^k - 1))) >> k, with truncation
               * toward zero. R11 is a flushed scratch register, while x
               * remains live in its assigned preg. LEA replaces the usual
               * copy-to-RAX plus ADD pair.
               */
              if (!mach_mov_preg_preg(e, 11, direct_dividend_preg) ||
                  !mach_rex(e, true, false, false, true) || !mach_u8(e, 0xc1) ||
                  !mach_u8(e, pow2_shift == 1 ? 0xeb : 0xfb) || !mach_u8(e, 63))
                return false;
              if (pow2_shift > 1 && (!mach_rex(e, true, false, false, true) ||
                                     !mach_u8(e, 0x81) || !mach_u8(e, 0xe3) ||
                                     !mach_i32(e, (int32_t)mask)))
                return false;
              /*
               * lea rax, [direct_dividend_preg + r11]. In SIB addressing,
               * base encodings 5 (RBP/R13) with mod=00 mean no base and
               * require a disp32. Use mod=01 plus disp8=0 for those bases.
               */
              bool needs_zero_disp8 = (direct_dividend_preg & 7) == 5;
              if (!mach_rex(e, true, false, true, direct_dividend_preg >= 8) ||
                  !mach_u8(e, 0x8d) ||
                  !mach_u8(e, needs_zero_disp8 ? 0x44 : 0x04) ||
                  !mach_u8(e, (unsigned char)(0x00 | (3 << 3) |
                                              (direct_dividend_preg & 7))) ||
                  (needs_zero_disp8 && !mach_u8(e, 0)) ||
                  !mach_rex(e, true, false, false, false) ||
                  !mach_u8(e, 0xc1) || !mach_u8(e, 0xf8) ||
                  !mach_u8(e, pow2_shift) ||
                  !mach_commit_vreg(e, &in->dst, dst))
                return false;
              break;
            }
            if (!mach_mov_preg_preg(e, 11, 0))
              return false;
            if (pow2_shift == 1) {
              /*
               * For division by two, the signed-truncation bias is exactly
               * the dividend's sign bit.  A logical shift computes that
               * 0/1 value directly, avoiding SAR plus AND.
               */
              if (!mach_rex(e, true, false, false, true) || !mach_u8(e, 0xc1) ||
                  !mach_u8(e, 0xeb) || !mach_u8(e, 63))
                return false;
            } else if (!mach_rex(e, true, false, false, true) ||
                       !mach_u8(e, 0xc1) || !mach_u8(e, 0xfb) ||
                       !mach_u8(e, 63) ||
                       !mach_rex(e, true, false, false, true) ||
                       !mach_u8(e, 0x81) || !mach_u8(e, 0xe3) ||
                       !mach_i32(e, (int32_t)mask)) {
              return false;
            }
            /*
             * add rax, r11
             */
            if (!mach_rex(e, true, true, false, false) || !mach_u8(e, 0x01) ||
                !mach_u8(e, 0xd8) || !mach_rex(e, true, false, false, false) ||
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
        int64_t sdivisor = 0;
        bool sdivisor_known = mach_operand_const_i64(e, &in->src1, &sdivisor);
        bool mersenne31 = in->opcode == NYR_MACH_MOD && sdivisor_known &&
                          sdivisor == INT64_C(2147483647);
        if (mersenne31) {
          /*
           * This sequence owns RAX, RDX, R8, and R9.  Values colored to
           * those registers must reach their canonical homes before the
           * sequence clobbers them; subsequent uses reload from those homes.
           */
          const unsigned mersenne_clobbers =
              (1u << 0) | (1u << 2) | (1u << 8) | (1u << 9);
          uint32_t dead_dividend =
              mach_dead_vreg_use_at_current_inst(e, &in->src0);
          if (!mach_flush_pregs_to_home_preserving_dead_use(
                  e, mersenne_clobbers, dead_dividend, false) ||
              !mach_load_op_rax(e, &in->src0) ||
              !mach_flush_pregs_to_home_preserving_dead_use(
                  e, mersenne_clobbers, dead_dividend, true) ||
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
          mach_record_div_stat(e, true);
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
             * A dividend proven in [0, 2^31) needs no sign correction: with
             * l = ceil(log2 d), s = 31 + l, m = ceil(2^s / d) (m <= 2^32),
             * q = (x * m) >> s exactly, and x * m < 2^63 so the 64-bit
             * product cannot overflow. Verified against C division on
             * hundreds of millions of (divisor, dividend) pairs.
             */
            if (in->dividend_u31 && ad < ((uint64_t)1 << 31) && !(negate)) {
              unsigned l = 0;
              while (((uint64_t)1 << l) < ad)
                ++l;
              unsigned shift = 31 + l;
              uint64_t um =
                  (uint64_t)((((unsigned __int128)1 << shift) + ad - 1) / ad);
              /*
               * Only RAX and RDX: q = (x * m) >> shift in RDX.
               */
              const unsigned fast_clobbers = (1u << 0) | (1u << 2) | (1u << 10);
              uint32_t fast_dead =
                  mach_dead_vreg_use_at_current_inst(e, &in->src0);
              if (!mach_flush_pregs_to_home_preserving_dead_use(
                      e, fast_clobbers, fast_dead, false) ||
                  !mach_load_op_rax(e, &in->src0) ||
                  !mach_flush_pregs_to_home_preserving_dead_use(
                      e, fast_clobbers, fast_dead, true) ||
                  /*
                   * movabs rdx, m ; imul rdx, rax ; shr rdx, shift
                   */
                  !mach_rex(e, true, false, false, false) ||
                  !mach_u8(e, 0xba) || !mach_i64(e, (int64_t)um) ||
                  !mach_alu_preg_preg(e, NYR_MACH_MUL, 2, 0) ||
                  !mach_rex(e, true, false, false, false) ||
                  !mach_u8(e, 0xc1) || !mach_u8(e, 0xea) ||
                  !mach_u8(e, (unsigned char)shift))
                return false;
              if (in->opcode == NYR_MACH_MOD) {
                /*
                 * rax = x - q * d, with q in rdx and x still in rax
                 */
                if (!mach_alu_preg_imm(e, NYR_MACH_MUL, 2, (int64_t)ad) ||
                    !mach_alu_preg_preg(e, NYR_MACH_SUB, 0, 2))
                  return false;
              } else if (!mach_mov_preg_preg(e, 0, 2)) {
                return false;
              }
              if (!mach_commit_vreg(e, &in->dst, dst))
                return false;
              mach_record_div_stat(e, true);
              break;
            }
            /*
             * Magic division in RAX/RDX with the dividend kept in the
             * reserved scratch R10 (the allocator only loses RDX):
             *   r10 = x ; rax = M ; imul r10        rdx:rax = x * M
             *   [add rdx, r10] ; sar rdx, s ; rdx -= x >> 63   q in rdx
             */
            const unsigned magic_clobbers = (1u << 0) | (1u << 2) | (1u << 10);
            uint32_t dead_dividend =
                mach_dead_vreg_use_at_current_inst(e, &in->src0);
            if (!mach_flush_pregs_to_home_preserving_dead_use(
                    e, magic_clobbers, dead_dividend, false) ||
                !mach_load_op_rax(e, &in->src0) ||
                !mach_flush_pregs_to_home_preserving_dead_use(
                    e, magic_clobbers, dead_dividend, true) ||
                !mach_mov_reg_rax(e, 10) ||
                !mach_rex(e, true, false, false, false) || !mach_u8(e, 0xb8) ||
                !mach_i64(e, magic) || !mach_rex(e, true, false, false, true) ||
                !mach_u8(e, 0xf7) || !mach_u8(e, 0xea))
              return false;
            if (magic < 0 && !mach_alu_preg_preg(e, NYR_MACH_ADD, 2, 10))
              return false;
            if (mshift > 0 &&
                (!mach_rex(e, true, false, false, false) || !mach_u8(e, 0xc1) ||
                 !mach_u8(e, 0xfa) || !mach_u8(e, (unsigned char)mshift)))
              return false;
            if (!mach_mov_preg_preg(e, 0, 10) ||
                !mach_rex(e, true, false, false, false) || !mach_u8(e, 0xc1) ||
                !mach_u8(e, 0xf8) || !mach_u8(e, 63) ||
                !mach_alu_preg_preg(e, NYR_MACH_SUB, 2, 0))
              return false;
            if (negate && (!mach_rex(e, true, false, false, false) ||
                           !mach_u8(e, 0xf7) || !mach_u8(e, 0xda)))
              return false;
            if (in->opcode == NYR_MACH_MOD) {
              if ((int64_t)(int32_t)ad == (int64_t)ad) {
                if (!mach_alu_preg_imm(e, NYR_MACH_MUL, 2, (int64_t)ad))
                  return false;
              } else if (!mach_rex(e, true, false, false, false) ||
                         !mach_u8(e, 0xb8) || !mach_i64(e, (int64_t)ad) ||
                         !mach_alu_preg_preg(e, NYR_MACH_MUL, 2, 0)) {
                return false;
              }
              if (!mach_mov_preg_preg(e, 0, 10) ||
                  !mach_alu_preg_preg(e, NYR_MACH_SUB, 0, 2))
                return false;
            } else if (!mach_mov_preg_preg(e, 0, 2)) {
              return false;
            }
            if (!mach_commit_vreg(e, &in->dst, dst))
              return false;
            mach_record_div_stat(e, true);
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
        mach_record_div_stat(e, false);
        break;
      case NYR_MACH_SELECT: {
        size_t fused_cmp = in->src0.kind == NYR_MACH_OPERAND_VREG &&
                                   e->select_cmp_inst &&
                                   in->src0.as.reg < e->mach->vreg_len
                               ? e->select_cmp_inst[in->src0.as.reg]
                               : SIZE_MAX;
        if (fused_cmp != SIZE_MAX &&
            e->mach->insts[fused_cmp].opcode == NYR_MACH_SUB) {
          const nyr_mach_operand_t *original = &e->mach->insts[fused_cmp].src1;
          int result = mach_vreg_def_preg(e, in->dst.as.reg);
          if (!mach_seed_colored_preg(e, original->as.reg))
            return false;
          int source = mach_vreg_preg(e, original->as.reg);
          if (source < 0) {
            source = 10;
            if (!mach_load_op_preg(e, original, (unsigned)source))
              return false;
          }
          if (result < 0 || source == result)
            result = 0;
          /*
           * neg sets SF from -x. Keep x when -x is negative. For INT64_MIN
           * both arms are INT64_MIN, so the wrapping result is unchanged.
           */
          if (!mach_mov_preg_preg(e, result, source) ||
              !mach_rex(e, true, false, false, result >= 8) ||
              !mach_u8(e, 0xf7) || !mach_u8(e, 0xd8 | (result & 7)) ||
              !mach_rex(e, true, result >= 8, false, source >= 8) ||
              !mach_u8(e, 0x0f) || !mach_u8(e, 0x48) ||
              !mach_u8(e, 0xc0 | ((result & 7) << 3) | (source & 7)))
            return false;
          if (result == 0) {
            mach_rax_invalidate(e);
            if (!mach_commit_vreg(e, &in->dst, dst))
              return false;
          } else if (!mach_commit_preg(e, &in->dst, dst, (unsigned)result,
                                       false))
            return false;
          break;
        }
        /*
         * Seed the true arm before establishing the predicate flags.
         * Rematerializing a zero in its assigned register uses XOR, which
         * would otherwise overwrite the bit-test flags below. Operand live
         * ranges overlap at this instruction, so subsequent operand loads
         * cannot reuse this register for a different live value.
         */
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
        if (fused_cmp != SIZE_MAX) {
          const nyr_mach_inst_t *cmp = &e->mach->insts[fused_cmp];
          int cc = mach_cmov_cc(cmp->condition);
          if (!mach_seed_colored_preg(e, in->src2.as.reg) ||
              !mach_seed_colored_preg(e, in->src1.as.reg) ||
              !mach_seed_colored_preg(e, cmp->src0.as.reg))
            return false;
          int64_t imm = 0;
          bool rhs_imm = mach_select_cmp_rhs_imm(e, &cmp->src1, &imm);
          int rhs_preg = -1;
          if (!rhs_imm) {
            if (!mach_seed_colored_preg(e, cmp->src1.as.reg))
              return false;
            rhs_preg = mach_vreg_preg(e, cmp->src1.as.reg);
          }
          /*
           * A clamp arm (`cond ? K : x`) is a proven int32 constant with no
           * allocated color, so it cannot be seeded or compared from a vreg.
           * Materialize it in the reserved encoder scratch (R10, never an
           * allocator color) before the compare: the zero form is XOR, which
           * would otherwise destroy the predicate flags. At most one arm is
           * constant here by construction in mach_prepare_select_bit_test.
           */
          int true_arm = mach_vreg_preg(e, in->src1.as.reg);
          int false_arm = mach_vreg_preg(e, in->src2.as.reg);
          int64_t true_imm = 0, false_imm = 0;
          if (true_arm < 0 &&
              mach_select_cmp_rhs_imm(e, &in->src1, &true_imm)) {
            if (!mach_mov_preg_imm(e, 10, true_imm))
              return false;
            true_arm = 10;
          }
          if (false_arm < 0 &&
              mach_select_cmp_rhs_imm(e, &in->src2, &false_imm)) {
            if (!mach_mov_preg_imm(e, 10, false_imm))
              return false;
            false_arm = 10;
          }
          int lhs_preg = mach_vreg_preg(e, cmp->src0.as.reg);
          int result_preg = mach_vreg_def_preg(e, in->dst.as.reg);
          if (cc < 0 || lhs_preg < 0 || true_arm < 0 || false_arm < 0 ||
              result_preg < 0 || (!rhs_imm && rhs_preg < 0))
            return mach_err(e, "x64 machine form encode: fused select "
                               "compare operands are not in registers");
          if (e->select_test_equal[in->src0.as.reg])
            cc ^= 1;
          if (rhs_imm) {
            bool small = imm >= -128 && imm <= 127;
            if (!mach_rex(e, true, false, false, lhs_preg >= 8) ||
                !mach_u8(e, small ? 0x83 : 0x81) ||
                !mach_u8(e, (unsigned char)(0xf8 | (lhs_preg & 7))) ||
                !(small ? mach_u8(e, (unsigned char)(imm & 0xff))
                        : mach_i32(e, (int32_t)imm)))
              return false;
          } else if (!mach_rex(e, true, rhs_preg >= 8, false, lhs_preg >= 8) ||
                     !mach_u8(e, 0x39) ||
                     !mach_u8(e, (unsigned char)(0xc0 | ((rhs_preg & 7) << 3) |
                                                 (lhs_preg & 7))))
            return false;
          /*
           * MOV keeps the flags.
           */
          int other_preg = true_arm;
          if (result_preg == true_arm) {
            cc ^= 1;
            other_preg = false_arm;
          } else if (!mach_mov_preg_preg(e, result_preg, false_arm)) {
            return false;
          }
          if (!mach_rex(e, true, result_preg >= 8, false, other_preg >= 8) ||
              !mach_u8(e, 0x0f) || !mach_u8(e, (unsigned char)(0x40 | cc)) ||
              !mach_u8(e, (unsigned char)(0xc0 | ((result_preg & 7) << 3) |
                                          (other_preg & 7))) ||
              !mach_commit_preg(e, &in->dst, dst, (unsigned)result_preg, false))
            return false;
          break;
        }
        int32_t fused_test_src = in->src0.kind == NYR_MACH_OPERAND_VREG &&
                                         e->select_test_src &&
                                         in->src0.as.reg < e->colors_len
                                     ? e->select_test_src[in->src0.as.reg]
                                     : -1;
        if (fused_test_src >= 0) {
          /*
           * Both arms are live at SELECT, so seed them before establishing
           * flags. Select directly into the allocated result register;
           * when it aliases the true arm, invert the condition instead of
           * destroying that arm with the initial false-value copy.
           */
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
            /*
             * Test the already-resident low bit without staging it in RAX.
             */
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
          /*
           * mach_rbp_disp writes the ModRM byte together with the
           * displacement (disp8: 45, disp32: 85). Emitting 85 here as well
           * added a stray byte, so the next instruction was swallowed into
           * the displacement and the stream desynchronized.
           */
          if (!mach_rex(e, true, false, false, false) || !mach_u8(e, 0x0f) ||
              !mach_u8(e, cmov_opcode) || !mach_rbp_disp(e, 0, true_slot))
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
            } else if (fold >= -128 && fold <= 127) {
              if (!mach_rex(e, true, false, false, false) ||
                  !mach_u8(e, 0x83) || !mach_u8(e, 0xf8) ||
                  !mach_u8(e, (unsigned char)fold))
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
        if (!mach_flush_float_to_home_mode(e, true) ||
            !mach_flush_vector_to_home(e, true))
          return false;
        /*
         * jmp to the target block (rel8 when it is a short backward edge)
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
        if (!mach_emit_branch_to(e, in->src1.as.block_index, 0))
          return false;
        break;
      case NYR_MACH_BR_IF:
        /*
         * XMM/vector values are caller-saved and must be materialized before
         * either branch edge. A resident GPR condition can be tested
         * directly; other conditions use their canonical home.
         */
        if (!mach_flush_float_to_home_mode(e, true) ||
            !mach_flush_vector_to_home(e, true))
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
          if (!mach_emit_branch_to(e, in->src1.as.block_index, jcc))
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
              !mach_emit_branch_to(e, in->src1.as.block_index, 0x85))
            return false;
          break;
        }
        /*
         * cmpq $0, mem; jne target
         */
        if (!mach_rex(e, true, false, false, false) || !mach_u8(e, 0x83) ||
            !mach_rbp_disp(e, 7, a) || !mach_u8(e, 0x00) ||
            !mach_emit_branch_to(e, in->src1.as.block_index, 0x85))
          return false;
        break;
      case NYR_MACH_CALL: {
        bool direct_call =
            in->src0.kind == NYR_MACH_OPERAND_SYMBOL && in->src0.as.symbol;
        if (!direct_call && in->src0.kind != NYR_MACH_OPERAND_VREG)
          return mach_err(e, "x64 machine form encode: call needs target");
        ny_native_runtime_op_t runtime_operation =
            direct_call
                ? (in->runtime_operation != NY_NATIVE_RUNTIME_OP_NONE
                       ? in->runtime_operation
                       : ny_native_runtime_operation(in->src0.as.symbol))
                : NY_NATIVE_RUNTIME_OP_NONE;
        /*
         * An ordinary ABI call may pass a caller-saved value that dies at the
         * call. Keep such an argument resident, skip its home flush, and move
         * it directly to the ABI destination. The saved preg is only used
         * when it does not alias another argument's destination; all other
         * shapes keep the normal home/staging path. Aggregates are excluded
         * because their copy sequence borrows the argument scratch registers.
         */
        int call_arg_src_preg[NYIR_CALL_MAX_ARGS];
        bool call_arg_resident[NYIR_CALL_MAX_ARGS];
        int call_gp_dest[NYIR_CALL_MAX_ARGS];
        int call_arg_order[NYIR_CALL_MAX_ARGS];
        size_t call_arg_order_n = 0;
        uint32_t call_skip_mask = 0;
        for (size_t ai = 0; ai < NYIR_CALL_MAX_ARGS; ++ai) {
          call_arg_src_preg[ai] = -1;
          call_arg_resident[ai] = false;
          call_gp_dest[ai] = -1;
          call_arg_order[ai] = -1;
        }
        if (direct_call && runtime_operation == NY_NATIVE_RUNTIME_OP_NONE &&
            in->args_len <= NYIR_CALL_MAX_ARGS && in->args) {
          bool has_aggregate = false;
          mach_call_gp_arg_dests(e, in, call_gp_dest, &has_aggregate);
          if (!has_aggregate) {
            bool candidate[NYIR_CALL_MAX_ARGS] = {0};
            size_t candidate_n = 0;
            for (size_t ai = 0; ai < in->args_len; ++ai) {
              const nyr_mach_operand_t *arg = &in->args[ai];
              if (arg->kind != NYR_MACH_OPERAND_VREG || call_gp_dest[ai] < 0)
                continue;
              uint32_t vreg = arg->as.reg;
              int preg = mach_vreg_preg(e, vreg);
              bool caller_saved =
                  preg >= 0 && preg != 3 && !(preg >= 12 && preg <= 15);
              const nyr_mach_live_segment_t *seg =
                  mach_segment_at(e, vreg, e->cur_inst);
              if (!caller_saved || !seg || seg->end != e->cur_inst ||
                  mach_vreg_used_in_later_block(e, vreg))
                continue;
              call_arg_src_preg[ai] = preg;
              candidate[ai] = true;
              ++candidate_n;
            }
            /*
             * Order the direct moves so a destination never overwrites a
             * source that a later direct move still needs. A cycle cannot be
             * resolved without a temporary, so fall back to the normal
             * flushed-home path for every candidate in that rare case.
             */
            bool done[NYIR_CALL_MAX_ARGS] = {0};
            bool cycle = false;
            for (size_t k = 0; k < candidate_n; ++k) {
              int pick = -1;
              for (size_t ai = 0; ai < in->args_len; ++ai) {
                if (!candidate[ai] || done[ai])
                  continue;
                bool noop = call_arg_src_preg[ai] == call_gp_dest[ai];
                bool blocked = false;
                if (!noop)
                  for (size_t aj = 0; aj < in->args_len; ++aj) {
                    if (aj == ai || !candidate[aj] || done[aj])
                      continue;
                    bool aj_noop = call_arg_src_preg[aj] == call_gp_dest[aj];
                    if (!aj_noop && call_arg_src_preg[aj] == call_gp_dest[ai]) {
                      blocked = true;
                      break;
                    }
                  }
                if (!blocked) {
                  pick = (int)ai;
                  break;
                }
              }
              if (pick < 0) {
                cycle = true;
                break;
              }
              done[pick] = true;
              call_arg_order[call_arg_order_n++] = pick;
            }
            if (cycle) {
              for (size_t ai = 0; ai < NYIR_CALL_MAX_ARGS; ++ai) {
                call_arg_src_preg[ai] = -1;
                call_arg_order[ai] = -1;
              }
              call_arg_order_n = 0;
            } else {
              for (size_t ai = 0; ai < in->args_len; ++ai) {
                if (!candidate[ai])
                  continue;
                int color = -1;
                for (int c = 0; c < MIR_COLOR_N; ++c)
                  if (mach_color_preg[c] == call_arg_src_preg[ai]) {
                    color = c;
                    break;
                  }
                if (color < 0)
                  continue;
                call_arg_resident[ai] = true;
                call_skip_mask |= UINT32_C(1) << (unsigned)color;
              }
            }
          }
        }
        /*
         * Publish ordinary colored values before the ABI call. Dedicated
         * loop values use their explicit canonical-home bridge. Dead
         * arguments selected above are intentionally left resident; their
         * placement consumes the register directly.
         */
        if (!mach_loop_gprs_call_home(e, false, true) ||
            !mach_flush_colored_mask_to_home(e, mach_call_gpr_home_mask(e, in) &
                                                    ~call_skip_mask) ||
            !mach_flush_float_to_home_mode(e, false) ||
            !mach_flush_vector_to_home(e, false))
          return false;
        /*
         * Every vector value is homed for the call: clear the upper YMM
         * state so the callee's legacy-SSE code does not stall.
         */
        if (e->uses_ymm && !mach_vzeroupper(e))
          return false;
        /*
         * Place judged-dead resident arguments before any home loads or
         * staging can reuse their source registers. The order above only
         * reverses dependencies between these moves; all other argument
         * destinations are written afterwards.
         */
        for (size_t k = 0; k < call_arg_order_n; ++k) {
          int ai = call_arg_order[k];
          if (ai < 0 || !call_arg_resident[ai])
            continue;
          if (!mach_load_call_arg_preg(e, &in->args[ai],
                                       (unsigned)call_gp_dest[ai],
                                       call_arg_src_preg[ai]))
            return false;
        }
        /*
         * Bare runtime operations end in mach_bare_call_finish, which
         * reloads every dedicated loop register from its home. The call
         * bridge above skipped callee-saved ones (an ABI call preserves
         * them), so publish those too or the reload restores a stale home:
         * a loop counter in r12 was reset to 0 after each bare print and
         * `while i < 12 { s = s + s; i += 1 }` never ended.
         */
        if (direct_call && e->bare_syscalls && e->target &&
            e->target->syscalls.available &&
            !mach_loop_gprs_call_home(e, false, false))
          return false;
        if (direct_call && e->bare_syscalls && e->target &&
            e->target->syscalls.available &&
            in->runtime_operation ==
                NY_NATIVE_RUNTIME_OP_TBUF_REGISTER_STATIC_HANDLE) {
          if (in->args_len != 1 || !mach_load_call_arg_rax(e, &in->args[0]))
            return mach_err(
                e, "x64 bare static buffer registration expects one argument");
          if (!mach_bare_call_finish(e, in))
            return false;
          break;
        }
        if (direct_call && e->bare_syscalls && e->target &&
            e->target->syscalls.available &&
            ny_bare_payload_op(in->runtime_operation, NULL)) {
          if (!mach_bare_payload(e, in))
            return false;
          break;
        }
        if (direct_call && e->bare_syscalls && e->target &&
            e->target->syscalls.available &&
            in->runtime_operation == NY_NATIVE_RUNTIME_OP_TYPED_BUFFER_NEW) {
          if (!mach_bare_tbuf_new(e, in))
            return false;
          break;
        }
        if (direct_call && e->bare_syscalls && e->target &&
            e->target->syscalls.available &&
            in->runtime_operation == NY_NATIVE_RUNTIME_OP_TYPED_BUFFER_LEN) {
          if (!mach_bare_tbuf_len(e, in))
            return false;
          break;
        }
        if (direct_call && e->bare_syscalls && e->target &&
            e->target->syscalls.available &&
            in->runtime_operation ==
                NY_NATIVE_RUNTIME_OP_TYPED_BUFFER_INDEX_READ) {
          if (!mach_bare_tbuf_index_read(e, in))
            return false;
          break;
        }
        if (direct_call && e->bare_syscalls && e->target &&
            e->target->syscalls.available &&
            in->runtime_operation == NY_NATIVE_RUNTIME_OP_TYPED_BUFFER_POP) {
          if (!mach_bare_tbuf_pop(e, in))
            return false;
          break;
        }
        if (direct_call && e->bare_syscalls && e->target &&
            e->target->syscalls.available &&
            (runtime_operation == NY_NATIVE_RUNTIME_OP_BARE_HEAP_ALLOC ||
             runtime_operation == NY_NATIVE_RUNTIME_OP_BARE_HEAP_FREE ||
             runtime_operation == NY_NATIVE_RUNTIME_OP_BARE_UNTAG)) {
          if (!mach_bare_heap(e, in))
            return false;
          break;
        }
        if (direct_call && e->bare_syscalls && e->target &&
            e->target->syscalls.available) {
          ny_native_runtime_op_t operation = runtime_operation;
          bool raw_memory = operation == NY_NATIVE_RUNTIME_OP_MEMSET ||
                            operation == NY_NATIVE_RUNTIME_OP_MEMCPY ||
                            operation == NY_NATIVE_RUNTIME_OP_MEMCMP;
          if (raw_memory) {
            if (!mach_bare_raw_memory(e, in, operation))
              return false;
            break;
          }
        }
        if (direct_call && e->bare_syscalls && e->target &&
            e->target->syscalls.available &&
            runtime_operation == NY_NATIVE_RUNTIME_OP_BOUNDS_TRAP) {
          if (!mach_bare_bounds_trap(e, in))
            return false;
          break;
        }
        if (direct_call && e->bare_syscalls && e->target &&
            e->target->syscalls.available) {
          ny_native_runtime_op_t operation = runtime_operation;
          bool stderr_integer =
              operation == NY_NATIVE_RUNTIME_OP_PRINT_I64_STDERR;
          bool stderr_newline =
              operation == NY_NATIVE_RUNTIME_OP_PRINT_NEWLINE_STDERR;
          bool stderr_cstr =
              operation == NY_NATIVE_RUNTIME_OP_PRINT_CSTR_STDERR;
          bool cstr_concat = operation == NY_NATIVE_RUNTIME_OP_CSTR_CONCAT;
          bool str_repeat = operation == NY_NATIVE_RUNTIME_OP_STR_REPEAT;
          bool str_chr = operation == NY_NATIVE_RUNTIME_OP_STR_CHR;
          bool cstr_equal = operation == NY_NATIVE_RUNTIME_OP_CSTR_EQUAL;
          bool cstr_compare = operation == NY_NATIVE_RUNTIME_OP_CSTR_COMPARE;
          bool emitted = cstr_concat    ? mach_bare_cstr_concat(e, in)
                         : str_repeat   ? mach_bare_str_repeat(e, in)
                         : str_chr      ? mach_bare_str_chr(e, in)
                         : cstr_equal   ? mach_bare_cstr_equal(e, in)
                         : cstr_compare ? mach_bare_cstr_compare(e, in)
                         : operation == NY_NATIVE_RUNTIME_OP_CSTR_TO_STRING
                             ? mach_bare_cstr_to_string(e, in)
                         : operation == NY_NATIVE_RUNTIME_OP_PRINT_CSTR
                             ? mach_bare_print_cstr(e, in, 1)
                         : stderr_cstr ? mach_bare_print_cstr(e, in, 2)
                         : operation == NY_NATIVE_RUNTIME_OP_CSTR_LEN
                             ? mach_bare_cstr_len(e, in)
                         : operation == NY_NATIVE_RUNTIME_OP_PRINT_I64
                             ? mach_bare_print_i64(e, in, 1)
                         : stderr_integer ? mach_bare_print_i64(e, in, 2)
                         : operation == NY_NATIVE_RUNTIME_OP_PRINT_NEWLINE
                             ? mach_bare_print_newline(e, in, 1)
                         : stderr_newline ? mach_bare_print_newline(e, in, 2)
                         : operation == NY_NATIVE_RUNTIME_OP_MONOTONIC_NS
                             ? mach_bare_monotonic_ns(e, in)
                         : operation == NY_NATIVE_RUNTIME_OP_BOOL_TO_CSTR
                             ? mach_bare_bool_to_cstr(e, in)
                             : false;
          if (operation == NY_NATIVE_RUNTIME_OP_CSTR_TO_STRING || cstr_concat ||
              str_repeat || str_chr || cstr_equal || cstr_compare ||
              operation == NY_NATIVE_RUNTIME_OP_PRINT_CSTR || stderr_cstr ||
              operation == NY_NATIVE_RUNTIME_OP_CSTR_LEN ||
              operation == NY_NATIVE_RUNTIME_OP_PRINT_I64 || stderr_integer ||
              operation == NY_NATIVE_RUNTIME_OP_PRINT_NEWLINE ||
              stderr_newline ||
              operation == NY_NATIVE_RUNTIME_OP_MONOTONIC_NS ||
              operation == NY_NATIVE_RUNTIME_OP_BOOL_TO_CSTR) {
            if (!emitted)
              return false;
            break;
          }
        }
        if (direct_call && e->target && e->target->syscalls.available &&
            runtime_operation == NY_NATIVE_RUNTIME_OP_LINUX_SYSCALL) {
          static const int syscall_arg_regs[] = {7, 6, 2, 10, 8, 9};
          if (in->args_len < 1 || in->args_len > 7)
            return mach_err(
                e, "x64 machine form encode: raw syscall needs 1-7 args");
          for (size_t ai = 1; ai < 7; ++ai) {
            if (ai < in->args_len) {
              if (!mach_load_call_arg_rax(e, &in->args[ai]) ||
                  !mach_mov_reg_rax(e, syscall_arg_regs[ai - 1]))
                return false;
            } else if (!mach_mov_preg_imm(e, (unsigned)syscall_arg_regs[ai - 1],
                                          0)) {
              return false;
            }
          }
          if (!mach_load_call_arg_rax(e, &in->args[0]) || !mach_u8(e, 0x0f) ||
              !mach_u8(e, 0x05))
            return false;
          mach_rax_invalidate(e);
          mach_r10_index_invalidate(e);
          if (in->dst.kind != NYR_MACH_OPERAND_NONE &&
              !mach_commit_vreg(e, &in->dst, mach_slot_off(e, &in->dst)))
            return false;
          break;
        }
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
        if (!mach_flush_float_to_home_mode(e, false) ||
            !mach_flush_vector_to_home(e, false))
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
            bool sticky_rax = mach_rax_tag_hit(e, off, (int)arg->as.reg) ||
                              mach_pending_rax_hit(e, off, (int)arg->as.reg);
            /*
             * A resident callee-saved color is independent of RAX's sticky
             * stack-home cache. Its valid preg was captured before the call
             * flush above. Only uncolored values need the cache check here.
             */
            bool callee_saved = sp == 3 || (sp >= 12 && sp <= 15);
            safe = callee_saved || call_arg_resident[ai] ||
                   (sp < 0 && !sticky_rax);
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
              !mach_rsp_disp(
                  e, 7,
                  (int32_t)(shadow + stack_i * 8)) || /* lea disp(%rsp),%rdi */
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
          if (!is_fp && call_arg_resident[ai]) {
            if (is_win)
              ++ordinal_n;
            else
              ++gp_n;
            continue;
          }
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
            } else if (!mach_load_call_arg_preg(e, &in->args[ai], (unsigned)reg,
                                                call_arg_resident[ai]
                                                    ? call_arg_src_preg[ai]
                                                    : -1)) {
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
                     direct_call ? in->src0.as.symbol : "<indirect>", ai);
            ny_dbg_abi_classify("call-arg", place, detail);
          }
          if (is_win)
            ++ordinal_n;
        }
        /*
         * Runtime entry points have fixed signatures, so %al is meaningless
         * for them; it was 8% of all emitted instructions.
         */
        bool known_fixed_arity =
            !in->call_is_variadic && direct_call &&
            ny_native_runtime_abi_decl(in->src0.as.symbol) != NULL;
        if ((in->call_is_variadic || in->call_is_extern) && !is_win &&
            !known_fixed_arity) {
          /*
           * SysV AMD64 variadic calls pass the number of XMM argument
           * registers used in %al. Set it after argument placement because
           * the placement path uses %rax as scratch.
           */
          if (fp_n > max_fp || !mach_u8(e, 0xb0) || !mach_u8(e, (unsigned)fp_n))
            return mach_err(e, "x64 machine form encode: invalid variadic "
                               "vector-register count");
          mach_rax_invalidate(e);
        }
        if (direct_call) {
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
          if (!mach_u8(e, 0xe8) ||
              !mach_add_reloc(e, call_sym, NY_RELOC_PLT32) || !mach_i32(e, 0))
            return false;
        } else {
          /*
           * Argument placement has already filled the argument registers. A
           * target allocated to one of them must come from its home, the
           * pre-call flush having stored it there.
           */
          bool in_arg_reg = false;
          if (in->src0.kind == NYR_MACH_OPERAND_VREG) {
            const nyr_mach_live_segment_t *target_seg =
                nyr_mach_regalloc_segment_at(&e->regalloc, in->src0.as.reg,
                                             e->cur_inst);
            in_arg_reg = target_seg && target_seg->color >= MIR_CALLEE_COLOR_N;
          }
          /*
           * Use r11 so staging the target cannot overwrite SysV %al.
           */
          if (!(in_arg_reg ? mach_load_home_preg(
                                 e, mach_vreg_home_off(e, in->src0.as.reg), 11)
                           : mach_load_op_preg(e, &in->src0, 11)))
            return false;
          if (mach->x64_inline_thunk) {
            if (!mach_bytes(e, nyr_x64_inline_retpoline,
                            sizeof(nyr_x64_inline_retpoline)))
              return false;
          } else if (mach->x64_indirect_thunk) {
            if (!mach_u8(e, 0xe8) ||
                !mach_add_reloc(e, "__x86_indirect_thunk_r11",
                                NY_RELOC_PLT32) ||
                !mach_i32(e, 0))
              return false;
          } else if (!mach_rex(e, false, false, false, true) ||
                     !mach_u8(e, 0xff) || !mach_u8(e, 0xd3)) {
            return false;
          }
        }
        if (stack_bytes) {
          if (!mach_rex(e, true, false, false, false) || !mach_u8(e, 0x81) ||
              !mach_u8(e, 0xc4) || !mach_i32(e, (int32_t)stack_bytes))
            return false;
        }
        /*
         * Argument placement can re-seed a caller-saved color after the
         * pre-call flush. Invalidate those colors, then restore dedicated
         * loop values from their canonical homes.
         */
        mach_invalidate_call_clobbered_colors(e);
        if (!mach_loop_gprs_call_home(e, true, true))
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
        if (e->uses_ymm && !mach_is_v128(mach, &in->src0) &&
            !mach_vzeroupper(e))
          return false;
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
        if (!mach_u8(e, 0x5d) || !mach_emit_return(e))
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
          if (strncmp(t, "db", 2) == 0 && t[2] >= '0' && t[2] <= '7' &&
              t[3] == '\0') {
            unsigned debug_reg = (unsigned)(t[2] - '0');
            if (in->src1.kind != NYR_MACH_OPERAND_VREG ||
                !mach_load_op_rax(e, &in->src1) || !mach_u8(e, 0x0f) ||
                !mach_u8(e, 0x23) || !mach_u8(e, 0xc0u | (debug_reg << 3)))
              return mach_err(
                  e, "x64 machine form encode: invalid debug-register write");
          } else if (t[0] == '\0') {
            /*
             * Empty volatile asm is a compiler barrier, not an instruction.
             */
          } else if (strcmp(t, "pause") == 0 || strcmp(t, "pause;") == 0) {
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
            if (e->err && e->err_len)
              snprintf(e->err, e->err_len,
                       "x64 machine form encode: unsupported inline asm: %s",
                       t);
            return false;
          }
        }
        break;
      case NYR_MACH_VA_SAVE: {
        /*
         * SysV register-save area used by va_start/va_arg.
         */
        static const unsigned gp_regs[] = {7, 6, 2, 1, 8, 9};
        if (!e->target || e->target->abi != NY_NATIVE_ABI_SYSV ||
            (in->immediate != 48 && in->immediate != 176) ||
            a > -in->immediate || in->src0.kind != NYR_MACH_OPERAND_FRAME)
          return mach_err(e,
                          "x64 machine form encode: unsupported va_save ABI");
        if (!mach_flush_colored_to_home(e) ||
            !mach_flush_float_to_home_mode(e, true) ||
            !mach_flush_vector_to_home(e, true))
          return false;
        for (size_t reg = 0; reg < sizeof(gp_regs) / sizeof(gp_regs[0]);
             ++reg) {
          unsigned preg = gp_regs[reg];
          if (!mach_rex(e, true, preg >= 8, false, false) ||
              !mach_u8(e, 0x89) || !mach_rbp_disp(e, preg, a + (int)(reg * 8)))
            return false;
        }
        for (unsigned reg = 0; in->immediate == 176 && reg < 8; ++reg)
          if (!mach_u8(e, 0x0f) || !mach_u8(e, 0x11) ||
              !mach_rbp_disp(e, reg, a + 48 + (int)(reg * 16)))
            return false;
        if (!mach_rex(e, true, false, false, false) || !mach_u8(e, 0x8d) ||
            !mach_rbp_disp(e, 0, a) || !mach_commit_vreg(e, &in->dst, dst))
          return false;
        mach_rax_invalidate(e);
        break;
      }
      case NYR_MACH_VA_START: {
        if (!e->target || e->target->abi != NY_NATIVE_ABI_SYSV ||
            in->src2.kind != NYR_MACH_OPERAND_IMM || in->immediate < 0 ||
            in->immediate > UINT32_MAX || in->src2.as.imm < 0 ||
            in->src2.as.imm > INT_MAX - 16)
          return mach_err(e, "x64 machine form encode: invalid va_start shape");
        unsigned gp_offset = (unsigned)in->immediate & 0xffffu;
        unsigned fp_offset = (unsigned)((uint64_t)in->immediate >> 16);
        if (gp_offset > 48 || fp_offset < 48 || fp_offset > 176)
          return mach_err(e,
                          "x64 machine form encode: invalid va_start offsets");
        if (!mach_flush_colored_to_home(e) ||
            !mach_flush_float_to_home_mode(e, true) ||
            !mach_flush_vector_to_home(e, true) || !mach_load_rax(e, a) ||
            !mach_rex(e, true, false, false, true) || !mach_u8(e, 0x89) ||
            !mach_u8(e, 0xc2) || !mach_load_rax(e, b) ||
            !mach_rex(e, true, false, false, true) || !mach_u8(e, 0x89) ||
            !mach_u8(e, 0xc3) || !mach_u8(e, 0x41) || !mach_u8(e, 0xc7) ||
            !mach_u8(e, 0x02) || !mach_i32(e, (int32_t)gp_offset) ||
            !mach_u8(e, 0x41) || !mach_u8(e, 0xc7) || !mach_u8(e, 0x42) ||
            !mach_u8(e, 0x04) || !mach_i32(e, (int32_t)fp_offset) ||
            !mach_rex(e, true, false, false, false) || !mach_u8(e, 0x8d) ||
            !mach_rbp_disp(e, 0, 16 + (int)in->src2.as.imm) ||
            !mach_rex(e, true, false, false, true) || !mach_u8(e, 0x89) ||
            !mach_u8(e, 0x42) || !mach_u8(e, 0x08) ||
            !mach_rex(e, true, true, false, true) || !mach_u8(e, 0x89) ||
            !mach_u8(e, 0x5a) || !mach_u8(e, 0x10))
          return false;
        mach_rax_invalidate(e);
        mach_r10_index_invalidate(e);
        break;
      }
      case NYR_MACH_VA_ARG: {
        if (!e->target || e->target->abi != NY_NATIVE_ABI_SYSV ||
            (in->immediate != 0 && in->immediate != 1))
          return mach_err(e, "x64 machine form encode: invalid va_arg shape");
        if (!mach_flush_colored_to_home(e) ||
            !mach_flush_float_to_home_mode(e, true) ||
            !mach_flush_vector_to_home(e, true) || !mach_load_rax(e, a) ||
            !mach_rex(e, true, false, false, true) || !mach_u8(e, 0x89) ||
            !mach_u8(e, 0xc2))
          return false;
        const bool fp = in->immediate == 1;
        const unsigned limit = fp ? 160u : 40u;
        const unsigned step = fp ? 16u : 8u;
        size_t stack_branch, done_branch;
        if (!mach_u8(e, 0x41) || !mach_u8(e, 0x8b) ||
            !mach_u8(e, fp ? 0x4a : 0x0a) || (fp && !mach_u8(e, 0x04)) ||
            (fp ? (!mach_u8(e, 0x81) || !mach_u8(e, 0xf9) ||
                   !mach_i32(e, (int32_t)limit))
                : (!mach_u8(e, 0x83) || !mach_u8(e, 0xf9) ||
                   !mach_u8(e, (unsigned char)limit))) ||
            !mach_u8(e, 0x0f) || !mach_u8(e, 0x87))
          return false;
        stack_branch = e->code.len;
        if (!mach_i32(e, 0) || !mach_u8(e, 0x4d) || !mach_u8(e, 0x8b) ||
            !mach_u8(e, 0x5a) || !mach_u8(e, 0x10) || !mach_u8(e, 0x49) ||
            !mach_u8(e, 0x01) || !mach_u8(e, 0xcb))
          return false;
        if (fp) {
          if (!mach_u8(e, 0xf2) || !mach_u8(e, 0x41) || !mach_u8(e, 0x0f) ||
              !mach_u8(e, 0x10) || !mach_u8(e, 0x03))
            return false;
        } else if (!mach_u8(e, 0x49) || !mach_u8(e, 0x8b) ||
                   !mach_u8(e, 0x03)) {
          return false;
        }
        if (!mach_u8(e, 0x41) || !mach_u8(e, 0x83) ||
            (fp ? (!mach_u8(e, 0x42) || !mach_u8(e, 0x04))
                : !mach_u8(e, 0x02)) ||
            !mach_u8(e, (unsigned char)step) || !mach_u8(e, 0xe9))
          return false;
        done_branch = e->code.len;
        if (!mach_i32(e, 0) ||
            !mach_patch_rel32(e, stack_branch, e->code.len) ||
            !mach_u8(e, 0x4d) || !mach_u8(e, 0x8b) || !mach_u8(e, 0x5a) ||
            !mach_u8(e, 0x08))
          return false;
        if (fp) {
          if (!mach_u8(e, 0xf2) || !mach_u8(e, 0x41) || !mach_u8(e, 0x0f) ||
              !mach_u8(e, 0x10) || !mach_u8(e, 0x03))
            return false;
        } else if (!mach_u8(e, 0x49) || !mach_u8(e, 0x8b) ||
                   !mach_u8(e, 0x03)) {
          return false;
        }
        if (!mach_u8(e, 0x49) || !mach_u8(e, 0x83) || !mach_u8(e, 0xc3) ||
            !mach_u8(e, 0x08) || !mach_u8(e, 0x4d) || !mach_u8(e, 0x89) ||
            !mach_u8(e, 0x5a) || !mach_u8(e, 0x08) ||
            !mach_patch_rel32(e, done_branch, e->code.len))
          return false;
        /*
         * Commit after advancing either cursor: the destination's register
         * may be R10 (va_list) or R11 (overflow argument address).
         */
        if (fp ? !mach_store_xmm(e, dst, false, 0)
               : !mach_commit_vreg(e, &in->dst, dst))
          return false;
        if (fp && in->dst.kind == NYR_MACH_OPERAND_VREG) {
          int preg = mach_vreg_fpreg(e, in->dst.as.reg);
          if (preg >= 0 && !mach_sse_mov_reg(e, (unsigned)preg, 0, false))
            return false;
          if (preg >= 0 && e->fp_color_seeded)
            e->fp_color_seeded[in->dst.as.reg] = true;
        }
        mach_rax_invalidate(e);
        mach_r10_index_invalidate(e);
        break;
      }
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
           * The copy only clobbers the physical registers it uses. Small
           * constant copies use RSI/RDI as base pointers; REP MOVSB also
           * clobbers RCX. Preserve live values in those colors without
           * publishing unrelated callee-saved and caller-saved registers.
           *
           * The two pointers are read from their canonical homes below, so
           * every register-resident value must be published first, not only
           * the RSI/RDI colors this sequence overwrites: the operands are
           * usually fresh `base + index * size` addresses living in other
           * colors, and their homes were never written. `next[h] = tbl[j]`
           * (dict-set) then copied through garbage pointers.
           */
          if (!mach_flush_colored_to_home(e))
            return false;
          int dstp = mach_slot_off(e, &in->src0);
          int srcp = mach_slot_off(e, &in->src1);
          if (!mach_load_rax(e, srcp) ||
              !mach_rex(e, true, false, false, false) || !mach_u8(e, 0x89) ||
              !mach_u8(e, 0xc6) || !mach_load_rax(e, dstp) ||
              !mach_rex(e, true, false, false, false) || !mach_u8(e, 0x89) ||
              !mach_u8(e, 0xc7))
            return false;
          if (nbytes <= 64) {
            /*
             * Small constant copy: straight-line moves through rax. `rep
             * movsb` costs tens of cycles to start, which dominates copies
             * of a few bytes. Offsets stay within disp8.
             */
            int64_t off = 0, rem = nbytes;
            while (rem > 0) {
              unsigned size = rem >= 8   ? 8u
                              : rem >= 4 ? 4u
                              : rem >= 2 ? 2u
                                         : 1u;
              unsigned char d8 = (unsigned char)off;
              bool ok;
              if (size == 8)
                ok = mach_u8(e, 0x48) && mach_u8(e, 0x8b) && mach_u8(e, 0x46) &&
                     mach_u8(e, d8) && mach_u8(e, 0x48) && mach_u8(e, 0x89) &&
                     mach_u8(e, 0x47) && mach_u8(e, d8);
              else if (size == 4)
                ok = mach_u8(e, 0x8b) && mach_u8(e, 0x46) && mach_u8(e, d8) &&
                     mach_u8(e, 0x89) && mach_u8(e, 0x47) && mach_u8(e, d8);
              else if (size == 2)
                ok = mach_u8(e, 0x0f) && mach_u8(e, 0xb7) && mach_u8(e, 0x46) &&
                     mach_u8(e, d8) && mach_u8(e, 0x66) && mach_u8(e, 0x89) &&
                     mach_u8(e, 0x47) && mach_u8(e, d8);
              else
                ok = mach_u8(e, 0x8a) && mach_u8(e, 0x46) && mach_u8(e, d8) &&
                     mach_u8(e, 0x88) && mach_u8(e, 0x47) && mach_u8(e, d8);
              if (!ok)
                return false;
              off += size;
              rem -= size;
            }
          } else if (!mach_rex(e, true, false, false, false) ||
                     !mach_u8(e, 0xc7) || !mach_u8(e, 0xc1) ||
                     !mach_i32(e, (int32_t)nbytes) || !mach_u8(e, 0xf3) ||
                     !mach_u8(e, 0xa4)) {
            return false;
          }
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
          (in->opcode == NYR_MACH_CONVERT || in->opcode == NYR_MACH_SQRT)) {
        unsigned clobbered =
            mach_convert_loop_fpr_clobbers(mach, in, e->loop_fpr);
        for (uint32_t v = 0; v < e->fp_colors_len; ++v)
          if (e->loop_fpr[v] >= 0 && (clobbered >> e->loop_fpr[v]) & 1u)
            e->fp_color_seeded[v] = false;
      }
      if (e->loop_fpr && in->dst.kind == NYR_MACH_OPERAND_VREG &&
          in->opcode != NYR_MACH_STORE && e->loop_fpr[in->dst.as.reg] >= 0) {
        uint32_t v = in->dst.as.reg;
        if (mach_loop_fpr_home_needed(e, v, bi, e->cur_inst) &&
            !mach_store_xmm(e, mach_vreg_home_off(e, v),
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
        e->last_rax_vreg = e->pending_rax_vreg;
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
  if (!e->suppress_stats)
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
  if (!e->suppress_stats)
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
  if (!e->suppress_stats)
    nyr_native_mach_vector_record(e->vec_regalloc.segment_len,
                                  vec_colored_segments, vec_spilled_segments,
                                  vec_reload_segments, vec_peak_live);
  /*
   * Audit here, while the per-class allocator results are still live and the
   * encoder's decision counters are final.  The outer callers run after this
   * function frees its allocators, which reported every regalloc field as
   * zero.
   */
  if (!e->suppress_stats)
    mach_audit_function(e, name);
  nyr_mach_regalloc_free(&e->vec_regalloc);
  nyr_mach_regalloc_free(&e->regalloc);
  nyr_mach_regalloc_free(&e->fp_regalloc);
  if (e->color_seeded && e->color_seeded != color_seeded_stack)
    free(e->color_seeded);
  e->color_seeded = NULL;
  if (e->fp_color_seeded && e->fp_color_seeded != fp_color_seeded_stack)
    free(e->fp_color_seeded);
  e->fp_color_seeded = NULL;
  free(e->fp_seed_inst);
  free(e->fp_last_def_inst);
  e->fp_seed_inst = e->fp_last_def_inst = NULL;
  if (e->vec_color_seeded && e->vec_color_seeded != vec_color_seeded_stack)
    free(e->vec_color_seeded);
  e->vec_color_seeded = NULL;
  if (e->home_store_counts && e->vreg_home_offs && mach->vreg_len) {
    e->home_offs_snapshot = malloc(mach->vreg_len * sizeof(int));
    if (e->home_offs_snapshot)
      memcpy(e->home_offs_snapshot, e->vreg_home_offs,
             mach->vreg_len * sizeof(int));
  }
  mach_free_remat_maps_preserving_offsets(e);
  free(e->def_inst);
  free(e->def_count);
  e->def_count = NULL;
  free(e->gpr_home_stored);
  e->gpr_home_stored = NULL;
  free(e->vec_home_stored);
  e->vec_home_stored = NULL;
  e->def_inst = NULL;
  /*
   * Patch relative jumps
   */
  if (!mach_fold_branch_trampolines(e))
    return false;
  for (size_t i = 0; i < e->patch_count; ++i) {
    size_t at = e->patch_at[i];
    uint32_t blk = e->patch_block[i];
    if (blk >= mach->block_len)
      return mach_err(e, "x64 machine form encode: bad branch target");
    if (e->patch_short && e->patch_short[i]) {
      int64_t short_rel = (int64_t)e->block_off[blk] - (int64_t)(at + 1);
      if (short_rel < -128 || short_rel > 127)
        return mach_err(e,
                        "x64 machine form encode: short branch out of range");
      e->code.data[at] = (unsigned char)(short_rel & 0xff);
      continue;
    }
    int32_t rel = (int32_t)((int64_t)e->block_off[blk] - (int64_t)(at + 4));
    e->code.data[at] = (unsigned char)(rel & 0xff);
    e->code.data[at + 1] = (unsigned char)((rel >> 8) & 0xff);
    e->code.data[at + 2] = (unsigned char)((rel >> 16) & 0xff);
    e->code.data[at + 3] = (unsigned char)((rel >> 24) & 0xff);
  }
  if (ny_dbg_is_enabled(NY_DBG_HEURISTIC) && ny_dbg_function_selected(name)) {
    ny_dbg_heuristic_event("X64_HOME_TRAFFIC",
                           "function=%s stores=%zu loads=%zu frame_ops=%zu "
                           "home_folds=%zu alu_commutes=%zu",
                           name ? name : "<unknown>", e->home_store_ops,
                           e->home_load_ops, e->frame_ops, e->home_fold_ops,
                           e->alu_commute_ops);
  }

  return true;
}

typedef struct {
  nyr_obj_buf_t code;
  nyr_x64_obj_reloc_t *relocs;
  size_t reloc_count;
  size_t *inst_offsets;
  size_t inst_offsets_count;
  bool success;
  char symbol[256];
  char error[256];
} ny_x64_parallel_encode_result_t;

typedef struct {
  const nyr_mach_func_t *funcs;
  const char *const *names;
  const size_t *order;
  const ny_native_target_info_t *target;
  bool bare_syscalls;
  bool home_dse;
  ny_x64_parallel_encode_result_t *results;
  size_t count;
} ny_x64_parallel_encode_ctx_t;

static unsigned char *
mach_home_dse_analyze(const nyr_mach_func_t *mach,
                      const ny_native_target_info_t *target, const char *symbol,
                      bool tag_return, bool bare_syscalls, size_t *limit_out,
                      nyr_x64_mach_enc_t *reuse, bool *reused);

static bool ny_x64_parallel_encode_task(size_t task, void *opaque) {
  ny_x64_parallel_encode_ctx_t *ctx = (ny_x64_parallel_encode_ctx_t *)opaque;
  /*
   * Workers pull tasks in index order and `order` is size-ascending, so the
   * largest function (usually rt_main) used to start last and finish alone
   * on the critical path. Start from the large end; each result keeps its
   * slot, so the object layout is unchanged.
   */
  size_t oi = ctx->count ? ctx->count - 1 - task : task;
  size_t i = ctx->order ? ctx->order[oi] : oi;
  ny_x64_parallel_encode_result_t *r = &ctx->results[oi];
  const char *nm = ctx->names && ctx->names[i] ? ctx->names[i] : "fn";
  snprintf(r->symbol, sizeof(r->symbol), NY_FMT_FN,
           ctx->target->symbol_prefix ? ctx->target->symbol_prefix : "", nm);
  if (!ctx->funcs[i].insts || ctx->funcs[i].inst_len == 0)
    return true; /* not lowered to machine form: caller falls back */
  nyr_mach_func_t mach_mut = ctx->funcs[i];
  (void)nyr_isle_apply_mach(&mach_mut);
  nyr_x64_mach_enc_t enc = {0};
  enc.mach = &mach_mut;
  enc.target = ctx->target;
  enc.err = r->error;
  enc.err_len = sizeof(r->error);
  enc.bare_syscalls = ctx->bare_syscalls;
  size_t dead_limit = 0;
  bool encoded = false;
  unsigned char *dead_homes =
      ctx->home_dse ? mach_home_dse_analyze(&mach_mut, ctx->target, r->symbol,
                                            false, ctx->bare_syscalls,
                                            &dead_limit, &enc, &encoded)
                    : NULL;
  if (!encoded) {
    enc.home_dead = dead_homes;
    enc.home_limit = dead_limit;
    encoded = mach_encode_function(&enc, r->symbol, false);
  }
  free(dead_homes);
  enc.home_dead = NULL;
  bool ok = encoded;
  if (ok) {
    r->inst_offsets = enc.inst_offsets;
    r->inst_offsets_count = mach_mut.inst_len;
    enc.inst_offsets = NULL;
  }
  mach_free_remat_maps(&enc);
  mach_enc_free_scratch(&enc);
  if (!ok) {
    if (getenv("NYTRIX_AOT_OBJECT_PROFILE"))
      fprintf(stderr, "[aot-object] phase=encode-failure symbol=%s reason=%s\n",
              r->symbol, r->error[0] ? r->error : "unspecified failure");
    mach_enc_dispose(&enc);
    return true;
  }
  r->code = enc.code;
  r->relocs = enc.relocs;
  r->reloc_count = enc.reloc_count;
  r->success = true;
  return true;
}

#include "code/backend/mach/arch/x86/object/home_dse.h"

bool nyr_x64_mach_append_function_debug(
    nyr_obj_buf_t *code, nyr_x64_obj_symbol_def_t *defs, size_t *def_count,
    nyr_x64_obj_reloc_t *relocs, size_t *reloc_count,
    const nyr_mach_func_t *mach, const ny_native_target_info_t *target,
    const char *symbol, bool tag_return, bool bare_syscalls,
    const nyr_func_t *source, nyr_native_debug_line_t **debug_lines,
    size_t *debug_line_count, size_t *debug_line_cap, char *err, size_t err_len,
    size_t **inst_offsets_out, size_t *inst_offsets_count) {
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
  size_t dead_limit = 0;
  nyr_x64_mach_enc_t enc = {0};
  bool encoded = false;
  unsigned char *dead_homes =
      mach_home_dse_analyze(&mach_mut, target, symbol, tag_return,
                            bare_syscalls, &dead_limit, &enc, &encoded);
  enc.mach = &mach_mut;
  enc.target = target;
  enc.bare_syscalls = bare_syscalls;
  enc.err = err;
  enc.err_len = err_len;
  if (!encoded) {
    enc.home_dead = dead_homes;
    enc.home_limit = dead_limit;
    encoded = mach_encode_function(&enc, symbol, tag_return);
  }
  free(dead_homes);
  enc.home_dead = NULL;
  if (!encoded) {
    mach_enc_dispose(&enc);
    return false;
  }
  if (!mach_align_function_start(code)) {
    nyr_set_error(err, err_len, "x86-64 machine form append: out of memory");
    mach_enc_dispose(&enc);
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
      const nyr_debug_loc_t *loc = &source->data[source_pc].debug;
      if (!nyr_native_debug_line_append(
              debug_lines, debug_line_count, debug_line_cap,
              (nyr_native_debug_line_t){*def_count, start + pc, loc->file,
                                        loc->line, loc->column})) {
        nyr_set_error(err, err_len,
                      "x86-64 machine form append: source map OOM");
        mach_enc_dispose(&enc);
        return false;
      }
    }
  }
  size_t *captured_offsets = enc.inst_offsets;
  size_t captured_count = enc.mach ? enc.mach->inst_len : 0;
  enc.inst_offsets = NULL;
  mach_free_remat_maps(&enc);
  if (*def_count >= NY_NATIVE_MAX_DEFS ||
      !nyr_obj_emit(code, enc.code.data, enc.code.len)) {
    nyr_set_error(err, err_len,
                  "x86-64 machine form append: object buffer is full");
    free(captured_offsets);
    mach_enc_dispose(&enc);
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
      free(captured_offsets);
      mach_enc_dispose(&enc);
      return false;
    }
    relocs[*reloc_count] = enc.relocs[i];
    relocs[*reloc_count].disp_off += start;
    (*reloc_count)++;
  }
  mach_enc_dispose(&enc);
  if (inst_offsets_out)
    *inst_offsets_out = captured_offsets;
  else
    free(captured_offsets);
  if (inst_offsets_count)
    *inst_offsets_count = captured_count;
  return true;
}

bool nyr_x64_mach_append_function(
    nyr_obj_buf_t *code, nyr_x64_obj_symbol_def_t *defs, size_t *def_count,
    nyr_x64_obj_reloc_t *relocs, size_t *reloc_count,
    const nyr_mach_func_t *mach, const ny_native_target_info_t *target,
    const char *symbol, bool tag_return, char *err, size_t err_len) {
  return nyr_x64_mach_append_function_debug(
      code, defs, def_count, relocs, reloc_count, mach, target, symbol,
      tag_return, false, NULL, NULL, NULL, NULL, err, err_len, NULL, NULL);
}

/*
 * Deterministic qsort input for the parallel encode order: size ascending,
 * ties keep the source index (identical to the previous stable sort).
 */
typedef struct {
  size_t inst_len;
  size_t idx;
} ny_x64_order_pair_t;

static int ny_x64_order_pair_cmp(const void *lhs, const void *rhs) {
  const ny_x64_order_pair_t *a = lhs;
  const ny_x64_order_pair_t *b = rhs;
  /*
   * Deterministic size-ascending order; ties keep the source index.
   */
  if (a->inst_len != b->inst_len)
    return a->inst_len < b->inst_len ? -1 : 1;
  if (a->idx != b->idx)
    return a->idx < b->idx ? -1 : 1;
  return 0;
}

typedef struct {
  uint64_t hash;
  size_t idx;
  bool used;
} ny_x64_outline_slot_t;

static uint64_t ny_x64_code_hash(const unsigned char *data, size_t len) {
  /*
   * FNV-1a 64; equality of bodies is still confirmed with memcmp.
   */
  uint64_t h = 1469598103934665603ULL;
  for (size_t i = 0; i < len; ++i) {
    h ^= data[i];
    h *= 1099511628211ULL;
  }
  return h;
}

static bool ny_x64_mach_build_bundle_impl(
    const nyr_mach_func_t *rt_main_mir, const nyr_mach_func_t *func_mirs,
    const char *const *func_names, size_t func_count,
    const ny_native_target_info_t *target, const char *entry_symbol,
    bool tag_return, bool bare_syscalls, bool home_dse, bool preserve_order,
    const nyr_func_t *rt_main_source, const nyr_func_t *func_sources,
    nyr_native_debug_line_t **debug_lines, size_t *debug_line_count,
    size_t *debug_line_cap, nyr_obj_buf_t *code, nyr_x64_obj_symbol_def_t *defs,
    size_t *def_count, nyr_x64_obj_reloc_t *relocs, size_t *reloc_count,
    bool append_tables, char *err, size_t err_len) {
  if (!rt_main_mir || !target || !code || !defs || !def_count || !relocs ||
      !reloc_count || !entry_symbol)
    return false;
  *def_count = 0;
  *reloc_count = 0;
  code->len = 0;
  bool profile_object = getenv("NYTRIX_AOT_OBJECT_PROFILE") != NULL;
  double encode_seconds = 0.0;
  double merge_seconds = 0.0;
  double entry_seconds = 0.0;
  ny_tick_t phase_start = 0;

  size_t *order = NULL;
  ny_x64_parallel_encode_result_t *results = NULL;
  ny_x64_outline_slot_t *outline_tab = NULL;
  if (func_count) {
    ny_x64_order_pair_t *pairs =
        preserve_order ? NULL : malloc(func_count * sizeof(*pairs));
    order = malloc(func_count * sizeof(*order));
    results = calloc(func_count, sizeof(*results));
    if ((!preserve_order && !pairs) || !order || !results) {
      free(pairs);
      free(order);
      free(results);
      return false;
    }
    if (preserve_order) {
      for (size_t i = 0; i < func_count; ++i)
        order[i] = i;
    } else {
      for (size_t i = 0; i < func_count; ++i) {
        pairs[i].inst_len = func_mirs[i].inst_len;
        pairs[i].idx = i;
      }
      /*
       * Deterministic size-ascending order; ties keep the source index.
       */
      qsort(pairs, func_count, sizeof(*pairs), ny_x64_order_pair_cmp);
      for (size_t i = 0; i < func_count; ++i)
        order[i] = pairs[i].idx;
    }
    free(pairs);
    size_t work = 0;
    for (size_t i = 0; i < func_count; ++i)
      work += func_mirs[i].inst_len;
    ny_x64_parallel_encode_ctx_t ctx = {
        .funcs = func_mirs,
        .names = func_names,
        .order = order,
        .target = target,
        .bare_syscalls = bare_syscalls,
        .home_dse = home_dse,
        .results = results,
        .count = func_count,
    };
    if (profile_object)
      phase_start = ny_ticks_now();
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
        free(results[i].inst_offsets);
      }
      free(order);
      free(results);
      return false;
    }
    if (profile_object)
      encode_seconds = ny_ticks_elapsed_sec(phase_start);
    size_t outline_buckets = 0;
    if (target->outline_machine_functions) {
      if (func_count > SIZE_MAX / 2) {
        nyr_set_error(err, err_len, "x86-64 outliner: function count overflow");
        goto outline_fail;
      }
      outline_buckets = 16;
      while (outline_buckets < func_count * 2) {
        if (outline_buckets > SIZE_MAX / 2) {
          nyr_set_error(err, err_len, "x86-64 outliner: bucket count overflow");
          goto outline_fail;
        }
        outline_buckets <<= 1;
      }
      outline_tab = calloc(outline_buckets, sizeof(*outline_tab));
      if (!outline_tab)
        goto outline_fail;
    }
    if (profile_object)
      phase_start = ny_ticks_now();
    for (size_t oi = 0; oi < func_count; ++oi) {
      ny_x64_parallel_encode_result_t *r = &results[oi];
      size_t source_index = order[oi];
      if (!r->success)
        continue;
      size_t outlined_to = SIZE_MAX;
      if (nyr_x64_obj_def_index(defs, *def_count, r->symbol) >= 0)
        continue;
      bool can_outline = !home_dse && target->outline_machine_functions &&
                         r->reloc_count == 0 && r->code.len > 5;
      uint64_t body_hash =
          can_outline ? ny_x64_code_hash(r->code.data, r->code.len) : 0;
      if (can_outline) {
        size_t slot = (size_t)body_hash & (outline_buckets - 1);
        for (;;) {
          ny_x64_outline_slot_t *s = &outline_tab[slot];
          if (!s->used)
            break;
          if (s->hash == body_hash) {
            const ny_x64_parallel_encode_result_t *candidate = &results[s->idx];
            if (candidate->reloc_count == 0 &&
                candidate->code.len == r->code.len &&
                memcmp(candidate->code.data, r->code.data, r->code.len) == 0) {
              outlined_to = s->idx;
              break;
            }
          }
          slot = (slot + 1) & (outline_buckets - 1);
        }
      }
      if (!mach_align_function_start(code))
        goto outline_fail;
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
      size_t emitted_def = *def_count;
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
      if (func_sources && debug_lines && debug_line_count && debug_line_cap) {
        const nyr_func_t *source = &func_sources[source_index];
        for (size_t mi = 0; mi < func_mirs[source_index].inst_len &&
                            mi < r->inst_offsets_count;
             ++mi) {
          size_t pc = r->inst_offsets ? r->inst_offsets[mi] : SIZE_MAX;
          uint32_t source_pc = func_mirs[source_index].insts[mi].source_pc;
          if (pc == SIZE_MAX || source_pc >= source->len ||
              !source->data[source_pc].debug.line)
            continue;
          if (pc > SIZE_MAX - start) {
            nyr_set_error(err, err_len,
                          "x86-64 machine bundle: source offset overflow");
            goto outline_fail;
          }
          const nyr_debug_loc_t *loc = &source->data[source_pc].debug;
          if (!nyr_native_debug_line_append(
                  debug_lines, debug_line_count, debug_line_cap,
                  (nyr_native_debug_line_t){emitted_def, start + pc, loc->file,
                                            loc->line, loc->column})) {
            nyr_set_error(err, err_len,
                          "x86-64 machine bundle: source map OOM");
            goto outline_fail;
          }
        }
      }
      /*
       * Register this body as a future outline candidate, matching the
       * previous scan which considered every prior result (outlined ones
       * included, keeping existing thunk-to-thunk chains valid).
       */
      if (can_outline) {
        size_t slot = (size_t)body_hash & (outline_buckets - 1);
        while (outline_tab[slot].used)
          slot = (slot + 1) & (outline_buckets - 1);
        outline_tab[slot].hash = body_hash;
        outline_tab[slot].idx = oi;
        outline_tab[slot].used = true;
      }
    }
    if (profile_object)
      merge_seconds = ny_ticks_elapsed_sec(phase_start);
    for (size_t i = 0; i < func_count; ++i) {
      nyr_obj_free(&results[i].code);
      free(results[i].relocs);
      free(results[i].inst_offsets);
    }
    free(outline_tab);
    outline_tab = NULL;
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
    free(results[i].inst_offsets);
  }
  free(outline_tab);
  free(order);
  free(results);
  return false;

outlined_functions_done:;
  if (profile_object)
    phase_start = ny_ticks_now();
  char entry[256];
  snprintf(entry, sizeof(entry), "%s%s",
           target->symbol_prefix ? target->symbol_prefix : "", entry_symbol);
  nyr_mach_func_t main_mut = *rt_main_mir;
  (void)nyr_isle_apply_mach(&main_mut);
  nyr_x64_mach_enc_t enc = {0};
  enc.mach = &main_mut;
  enc.target = target;
  enc.bare_syscalls = bare_syscalls;
  enc.err = err;
  enc.err_len = err_len;
  if (!mach_align_function_start(code)) {
    mach_enc_dispose(&enc);
    return false;
  }
  size_t start = code->len;
  size_t dead_limit = 0;
  bool encoded = false;
  unsigned char *dead_homes =
      home_dse
          ? mach_home_dse_analyze(&main_mut, target, entry, tag_return,
                                  bare_syscalls, &dead_limit, &enc, &encoded)
          : NULL;
  if (!encoded) {
    enc.home_dead = dead_homes;
    enc.home_limit = dead_limit;
    encoded = mach_encode_function(&enc, entry, tag_return);
  }
  free(dead_homes);
  enc.home_dead = NULL;
  if (!encoded) {
    mach_enc_dispose(&enc);
    return false;
  }
  mach_free_remat_maps(&enc);
  if (!nyr_obj_emit(code, enc.code.data, enc.code.len) ||
      *def_count >= NY_NATIVE_MAX_DEFS) {
    mach_enc_dispose(&enc);
    return false;
  }
  snprintf(defs[*def_count].name, sizeof(defs[*def_count].name), "%s", entry);
  defs[*def_count].off = start;
  defs[*def_count].size = enc.code.len;
  size_t entry_def = *def_count;
  (*def_count)++;
  if (rt_main_source && debug_lines && debug_line_count && debug_line_cap) {
    for (size_t mi = 0; mi < main_mut.inst_len; ++mi) {
      size_t pc = enc.inst_offsets ? enc.inst_offsets[mi] : SIZE_MAX;
      uint32_t source_pc = main_mut.insts[mi].source_pc;
      if (pc == SIZE_MAX || source_pc >= rt_main_source->len ||
          !rt_main_source->data[source_pc].debug.line)
        continue;
      if (pc > SIZE_MAX - start) {
        nyr_set_error(err, err_len,
                      "x86-64 machine bundle: entry source offset overflow");
        mach_enc_dispose(&enc);
        return false;
      }
      const nyr_debug_loc_t *loc = &rt_main_source->data[source_pc].debug;
      if (!nyr_native_debug_line_append(
              debug_lines, debug_line_count, debug_line_cap,
              (nyr_native_debug_line_t){entry_def, start + pc, loc->file,
                                        loc->line, loc->column})) {
        nyr_set_error(err, err_len,
                      "x86-64 machine bundle: entry source map OOM");
        mach_enc_dispose(&enc);
        return false;
      }
    }
  }
  for (size_t r = 0; r < enc.reloc_count; ++r) {
    if (*reloc_count >= NY_X64_OBJ_MAX_RELOCS) {
      mach_enc_dispose(&enc);
      return false;
    }
    relocs[*reloc_count] = enc.relocs[r];
    relocs[*reloc_count].disp_off += start;
    (*reloc_count)++;
  }
  mach_enc_dispose(&enc);
  if (profile_object)
    entry_seconds = ny_ticks_elapsed_sec(phase_start);
  bool tables_ok = true;
  if (append_tables) {
    if (!nyr_strtab_append_defs(code, defs, def_count, err, err_len) ||
        !nyr_consttab_append_defs(code, defs, def_count, err, err_len) ||
        !nyr_globaltab_append_defs(code, defs, def_count, err, err_len))
      return false;
    tables_ok = nyr_arraytab_append_defs(code, defs, def_count, err, err_len);
  }
  if (profile_object) {
    fprintf(stderr,
            "[aot-object] phase=bundle-encode functions=%zu encode=%.6fs "
            "merge=%.6fs entry=%.6fs\n",
            func_count, encode_seconds, merge_seconds, entry_seconds);
  }
  return tables_ok;
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
  return ny_x64_mach_build_bundle_impl(
      rt_main_mir, func_mirs, func_names, func_count, target, entry_symbol,
      tag_return, false, false, false, NULL, NULL, NULL, NULL, NULL, code, defs,
      def_count, relocs, reloc_count, true, err, err_len);
}

bool nyr_x64_mach_build_bundle_debug(
    const nyr_mach_func_t *rt_main_mir, const nyr_mach_func_t *func_mirs,
    const char *const *func_names, size_t func_count,
    const ny_native_target_info_t *target, const char *entry_symbol,
    bool tag_return, bool bare_syscalls, const nyr_func_t *rt_main_source,
    const nyr_func_t *func_sources, nyr_obj_buf_t *code,
    nyr_x64_obj_symbol_def_t *defs, size_t *def_count,
    nyr_x64_obj_reloc_t *relocs, size_t *reloc_count,
    nyr_native_debug_line_t **debug_lines, size_t *debug_line_count,
    size_t *debug_line_cap, char *err, size_t err_len) {
  return ny_x64_mach_build_bundle_impl(
      rt_main_mir, func_mirs, func_names, func_count, target, entry_symbol,
      tag_return, bare_syscalls, true, true, rt_main_source, func_sources,
      debug_lines, debug_line_count, debug_line_cap, code, defs, def_count,
      relocs, reloc_count, false, err, err_len);
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
  unsigned char bytes[] = {0x55, 0x48, 0x89, 0xe5, 0x48, 0xb8, 0,    0,
                           0,    0,    0,    0,    0,    0,    0xc9, 0xc3};
  for (unsigned i = 0; i < 8; ++i)
    bytes[6 + i] = (unsigned char)((uint64_t)imm >> (i * 8));
  if (!nyr_obj_emit(code, bytes, sizeof(bytes)))
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
  unsigned char stencil[sizeof(bytes) + 8 + 10];
  memcpy(stencil, bytes, sizeof(bytes));
  for (unsigned i = 0; i < 8; ++i)
    stencil[sizeof(bytes) + i] = (unsigned char)((uint64_t)imm >> (i * 8));
  const unsigned char tail[] = {0x48, 0x89, 0x45, 0xf8, 0x48,
                                0x8b, 0x45, 0xf8, 0xc9, 0xc3};
  memcpy(stencil + sizeof(bytes) + 8, tail, sizeof(tail));
  if (!nyr_obj_emit(code, stencil, sizeof(stencil)))
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
