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
#include "code/native/object/internal.h"
#include "code/native/ir/machine.h"
#include "base/parallel.h"

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

typedef struct {
  ny_obj_buf_t code;
  const ny_mach_func_t *mach;
  const ny_native_target_info_t *target;
  size_t *block_off;
  size_t *patch_at;
  uint32_t *patch_block;
  size_t patch_count;
  size_t patch_cap;
  ny_x64_obj_reloc_t *relocs;
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
  ny_mach_regalloc_t regalloc;
  size_t colors_len;
  size_t cur_inst;
  bool *color_seeded;
  bool force_stack_reads; /* diagnostic fallback: ignore colored registers */
  bool no_sticky; /* disable sticky rax (multi-block safety) */
  unsigned char fused_jcc; /* flags-only CMP jcc opcode for the next BR_IF (0=none) */
  /*
   * Callee-save push count (r12-r15/rbx). Frame/vreg homes sit below them.
   */
  unsigned save_mask;
  int cs_slots;
  ny_mach_regalloc_t fp_regalloc;
  bool *fp_color_seeded;
  size_t fp_colors_len;
  bool fp_fast_path;
  ny_mach_regalloc_t vec_regalloc;
  bool *vec_color_seeded;
  size_t vec_colors_len;
  bool vec_fast_path;
  /*
   * Constant vregs are rematerialized at reload sites instead of read from spill homes.
   */
  const int64_t *imm_fold;
  const bool *imm_fold_known;
  const ny_mach_operand_t *addr_fold;
  const bool *addr_fold_known;
  /*
   * Type-sized, disjoint stack homes for vregs (negative rbp offsets).
   */
  int *vreg_home_offs;
  size_t vreg_home_bytes;
} ny_x64_mach_enc_t;

static void mach_free_remat_maps(ny_x64_mach_enc_t *e) {
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
}

static bool mach_err(ny_x64_mach_enc_t *e, const char *msg) {
  if (e && e->err && e->err_len)
    snprintf(e->err, e->err_len, "%s", msg ? msg : "mach encode failed");
  return false;
}

static bool mach_bytes(ny_x64_mach_enc_t *e, const unsigned char *p, size_t n) {
  if (!ny_obj_emit(&e->code, p, n))
    return mach_err(e, "x64 machine form encode: out of memory");
  return true;
}

static bool mach_u8(ny_x64_mach_enc_t *e, unsigned v) {
  unsigned char b = (unsigned char)v;
  return mach_bytes(e, &b, 1);
}
static bool mach_rex(ny_x64_mach_enc_t *e, bool w, bool r, bool x, bool b) {
  unsigned rex = 0x40u | (w ? 0x08u : 0) | (r ? 0x04u : 0) |
                 (x ? 0x02u : 0) | (b ? 0x01u : 0);
  return rex == 0x40u ? true : mach_u8(e, rex);
}

static bool mach_i32(ny_x64_mach_enc_t *e, int32_t v) {
  unsigned char b[4] = {(unsigned char)(v & 0xff),
                        (unsigned char)((uint32_t)v >> 8),
                        (unsigned char)((uint32_t)v >> 16),
                        (unsigned char)((uint32_t)v >> 24)};
  return mach_bytes(e, b, 4);
}

static bool mach_i64(ny_x64_mach_enc_t *e, int64_t v) {
  for (int i = 0; i < 8; ++i)
    if (!mach_u8(e, (unsigned)(((uint64_t)v >> (i * 8)) & 0xff)))
      return false;
  return true;
}

/*
 * Colors 0..4 are callee-saved GPRs (r12-r15/rbx).  Leaf machine functions
 * also admit rsi/rdi as colors 5..6: no call can clobber them, so keeping
 * them live avoids unnecessary spill homes.  Functions containing a call use
 * only the ABI-stable first five colors.
 */
static const int mach_color_preg[] = {12, 13, 14, 15, 3, 6, 7};
#define MIR_CALLEE_COLOR_N 5
#define MIR_COLOR_N ((int)(sizeof(mach_color_preg) / sizeof(mach_color_preg[0])))
/*
 * Stack layout below %rbp (after push %rbp; mov %rsp,%rbp):
 *   [rbp - 8*cs_slots .. rbp)     saved colors used by this function
 *   [next .. )                    frame slots [0..frame_slot_len)
 *   [after frame .. )             vreg spill homes
 * Vreg homes are packed by machine type: scalar/FPR values reserve 8 bytes,
 * V128 values 16, and V256 values 32. Vector loads/stores use unaligned-safe
 * forms, so 8-byte packing alignment is sufficient while still keeping every
 * home disjoint. Offsets skip the push area or ALLOCA/locals clobber saved
 * regs and each other (nested aggregate ABI regressions).
 */
static int mach_cs_count(unsigned mask) {
  int n = 0;
  for (int i = 0; i < MIR_CALLEE_COLOR_N; ++i)
    if (mask & (1u << i))
      ++n;
  return n;
}

static int mach_vreg_home_off(const ny_x64_mach_enc_t *e, uint32_t vreg) {
  if (!e || !e->mach || !e->vreg_home_offs || vreg >= e->mach->vreg_len)
    return 0;
  return e->vreg_home_offs[vreg];
}

static size_t mach_vreg_home_size(const ny_mach_func_t *mach, size_t vreg) {
  if (!mach || !mach->vreg_types || vreg >= mach->vreg_len)
    return 8;
  switch (mach->vreg_types[vreg]) {
  case NY_MACH_TYPE_V256_I64:
    return 32;
  case NY_MACH_TYPE_V128_I64:
  case NY_MACH_TYPE_V128_F64:
  case NY_MACH_TYPE_V128_F32:
    return 16;
  default:
    return 8;
  }
}

static bool mach_build_vreg_homes(ny_x64_mach_enc_t *e,
                                  size_t frame_slots_bytes) {
  if (!e || !e->mach)
    return false;
  const ny_mach_func_t *mach = e->mach;
  if (!mach->vreg_len) {
    e->vreg_home_bytes = 0;
    return true;
  }
  int *offs = calloc(mach->vreg_len, sizeof(*offs));
  if (!offs)
    return mach_err(e, "x64 machine form encode: OOM (vreg homes)");
  size_t used = 0;
  for (size_t v = 0; v < mach->vreg_len; ++v) {
    size_t width = mach_vreg_home_size(mach, v);
    if (used > SIZE_MAX - width || frame_slots_bytes > SIZE_MAX - used - width) {
      free(offs);
      return mach_err(e, "x64 machine form encode: vreg home overflow");
    }
    used += width;
    size_t below_rbp = (size_t)e->cs_slots * 8u + frame_slots_bytes + used;
    if (below_rbp > (size_t)INT_MAX) {
      free(offs);
      return mach_err(e, "x64 machine form encode: vreg home too deep");
    }
    offs[v] = -(int)below_rbp;
  }
  e->vreg_home_offs = offs;
  e->vreg_home_bytes = used;
  return true;
}
static int mach_slot_off(const ny_x64_mach_enc_t *e, const ny_mach_operand_t *op) {
  if (!e || !e->mach || !op)
    return 0;
  if (op->kind == NY_MACH_OPERAND_FRAME)
    return -8 * (e->cs_slots + (int)op->as.frame_index + 1);
  if (op->kind == NY_MACH_OPERAND_VREG)
    return mach_vreg_home_off(e, op->as.reg);
  return 0;
}

static void mach_rax_invalidate(ny_x64_mach_enc_t *e);
static bool mach_is_i64(const ny_mach_func_t *m,
                        const ny_mach_operand_t *op);
static bool mach_vreg_future_use_requires_home(
    const ny_x64_mach_enc_t *e, uint32_t vreg);
static bool mach_store_preg_home(ny_x64_mach_enc_t *e, int preg, int off);
static int mach_carried_vreg_preg(const ny_x64_mach_enc_t *e, uint32_t vreg);
static bool mach_vreg_can_forward_to_next_rax(const ny_x64_mach_enc_t *e,
                                              uint32_t vreg);

#define MFP_COLOR_N 8
#define MVEC_COLOR_N 8
#define MACH_SEEDED_STACK_MAX 64

static int mach_vreg_fpreg(const ny_x64_mach_enc_t *e, uint32_t vreg) {
  if (!e || !e->fp_fast_path || vreg >= e->fp_colors_len)
    return -1;
  const ny_mach_live_segment_t *seg =
      ny_mach_regalloc_segment_at(&e->fp_regalloc, vreg, e->cur_inst);
  if (!seg || seg->color < 0 || seg->color >= MFP_COLOR_N)
    return -1;
  return seg->color;
}

static int mach_vreg_vpreg(const ny_x64_mach_enc_t *e, uint32_t vreg) {
  if (!e || !e->vec_fast_path || vreg >= e->vec_colors_len)
    return -1;
  const ny_mach_live_segment_t *seg =
      ny_mach_regalloc_segment_at(&e->vec_regalloc, vreg, e->cur_inst);
  if (!seg || seg->color < 0 || seg->color >= MVEC_COLOR_N)
    return -1;
  return seg->color;
}

static const ny_mach_live_segment_t *mach_fsegment_at(
    const ny_x64_mach_enc_t *e, uint32_t vreg, size_t inst) {
  return e ? ny_mach_regalloc_segment_at(&e->fp_regalloc, vreg, inst) : NULL;
}

static bool mach_fsegment_carries(const ny_x64_mach_enc_t *e, uint32_t vreg,
                                  const ny_mach_live_segment_t *seg) {
  if (!e || !seg || seg->start == 0 || seg->reload || seg->color < 0)
    return false;
  const ny_mach_live_segment_t *prev =
      mach_fsegment_at(e, vreg, seg->start - 1);
  return prev && prev->end + 1 == seg->start && !prev->spill &&
         prev->color == seg->color;
}

static int mach_vreg_preg(const ny_x64_mach_enc_t *e, uint32_t vreg) {
  if (!e || vreg >= e->colors_len || e->force_stack_reads)
    return -1;
  const ny_mach_live_segment_t *seg =
      ny_mach_regalloc_segment_at(&e->regalloc, vreg, e->cur_inst);
  if (!seg || seg->color < 0 || seg->color >= MIR_COLOR_N)
    return -1;
  return mach_color_preg[seg->color];
}


static const ny_mach_live_segment_t *mach_segment_at(
    const ny_x64_mach_enc_t *e, uint32_t vreg, size_t inst) {
  return e ? ny_mach_regalloc_segment_at(&e->regalloc, vreg, inst) : NULL;
}

static bool mach_segment_carries(const ny_x64_mach_enc_t *e, uint32_t vreg,
                                 const ny_mach_live_segment_t *seg) {
  if (!e || !seg || seg->start == 0 || seg->reload || seg->color < 0)
    return false;
  const ny_mach_live_segment_t *prev =
      mach_segment_at(e, vreg, seg->start - 1);
  return prev && prev->end + 1 == seg->start && !prev->spill &&
         prev->color == seg->color;
}

/*
 * Map machine-form comparison condition to x86 setcc opcode.
 * Integer path uses signed setl/setle/setg/setge; float path uses
 * unordered-compatible setb/setbe/seta/setae after ucomisd.
 */
static unsigned char mach_cond_setcc(ny_mach_cond_t cond, bool is_float) {
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
  unsigned idx = (unsigned)cond - (unsigned)NY_MACH_COND_EQ;
  if (idx >= 6)
    return 0;
  return tab[idx];
}

/*
 * Map machine-form condition to x86 jcc rel32 opcode (0F 8x).  The float
 * table uses the unordered-compatible unsigned compares that mirror the
 * float setcc table after ucomis[sd].
 */
static unsigned char mach_cond_jcc(ny_mach_cond_t cond, bool is_float) {
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
  unsigned idx = (unsigned)cond - (unsigned)NY_MACH_COND_EQ;
  if (idx >= 6)
    return 0;
  return tab[idx];
}

static bool mach_begin_segments(ny_x64_mach_enc_t *e) {
  if (!e || !e->color_seeded)
    return true;
  for (uint32_t v = 0; v < e->colors_len; ++v) {
    const ny_mach_live_segment_t *seg = mach_segment_at(e, v, e->cur_inst);
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
     * being proven resident in this block.
     */
    e->color_seeded[v] =
        (carried && e->color_seeded[v]) ||
        (e->color_seeded[v] && mach_carried_vreg_preg(e, v) >= 0);
  }
  return true;
}

static bool mach_block_unconditional_self_loop(const ny_x64_mach_enc_t *e,
                                              size_t block) {
  if (!e || !e->mach || block >= e->mach->block_len)
    return false;
  const ny_mach_block_t *b = &e->mach->blocks[block];
  if (!b->inst_count)
    return false;
  const ny_mach_inst_t *last = &e->mach->insts[b->first_inst + b->inst_count - 1];
  return last->opcode == NY_MACH_BR &&
         last->src1.kind == NY_MACH_OPERAND_BLOCK &&
         last->src1.as.block_index == block;
}

static size_t mach_block_for_inst(const ny_x64_mach_enc_t *e, size_t inst) {
  if (!e || !e->mach)
    return SIZE_MAX;
  for (size_t block = 0; block < e->mach->block_len; ++block) {
    const ny_mach_block_t *b = &e->mach->blocks[block];
    if (inst >= b->first_inst && inst - b->first_inst < b->inst_count)
      return block;
  }
  return SIZE_MAX;
}

static bool mach_end_segments(ny_x64_mach_enc_t *e) {
  if (!e || !e->color_seeded)
    return true;
  for (uint32_t v = 0; v < e->colors_len; ++v) {
    const ny_mach_live_segment_t *seg = mach_segment_at(e, v, e->cur_inst);
    if (!seg || seg->end != e->cur_inst)
      continue;
    const ny_mach_live_segment_t *next =
        mach_segment_at(e, v, e->cur_inst + 1);
    size_t block = mach_block_for_inst(e, e->cur_inst);
    bool preserve_self_loop =
        block != SIZE_MAX &&
        mach_block_unconditional_self_loop(e, block) &&
        ny_mach_regalloc_live_in(&e->regalloc, block, v) &&
        !seg->spill;
    /*
     * A carried value continues in its preg across the edge: no home store
     * needed unless it spills (live-out headers still home for the exit
     * path, which reloads from the canonical stack slot).
     */
    bool needs_home =
        !preserve_self_loop &&
        (seg->spill ||
         (!seg->carried && next && !mach_segment_carries(e, v, next)));
    if (needs_home && seg->color >= 0 && e->color_seeded[v]) {
      int preg = mach_color_preg[seg->color];
      int off = mach_vreg_home_off(e, v);
      if (!mach_store_preg_home(e, preg, off))
        return false;
      mach_rax_invalidate(e);
    }
    if (seg->spill ||
        (!preserve_self_loop && !seg->carried &&
         !mach_segment_at(e, v, e->cur_inst + 1)))
      e->color_seeded[v] = false;
  }
  return true;
}

static bool mach_add_reloc(ny_x64_mach_enc_t *e, const char *sym, int type);

static bool mach_remat_addr_preg(ny_x64_mach_enc_t *e, int preg,
                                  const ny_mach_operand_t *addr) {
  if (!e || !addr || preg < 0)
    return false;
  if (addr->kind == NY_MACH_OPERAND_FRAME) {
    unsigned char modrm = (unsigned char)(0x85 | ((preg & 7) << 3));
    return mach_rex(e, true, preg >= 8, false, false) &&
           mach_u8(e, 0x8d) && mach_u8(e, modrm) &&
           mach_i32(e, mach_slot_off(e, addr));
  }
  if (addr->kind == NY_MACH_OPERAND_SYMBOL && addr->as.symbol) {
    unsigned char modrm = (unsigned char)(0x05 | ((preg & 7) << 3));
    return mach_rex(e, true, preg >= 8, false, false) &&
           mach_u8(e, 0x8d) && mach_u8(e, modrm) &&
           mach_add_reloc(e, addr->as.symbol, NY_RELOC_PC32) && mach_i32(e, 0);
  }
  return false;
}

/*
 * If preg not yet proven in this block, reload from dual-written stack home.
 */
static bool mach_seed_colored_preg(ny_x64_mach_enc_t *e, uint32_t vreg) {
  if (!e || vreg >= e->colors_len)
    return true;
  int preg = mach_vreg_preg(e, vreg);
  if (preg < 0)
    return true;
  if (e->color_seeded && e->color_seeded[vreg])
    return true;
  int off = mach_vreg_home_off(e, vreg);
  if (e->imm_fold && e->imm_fold_known &&
      e->imm_fold_known[vreg]) {
    /*
     * movabsq $imm64, %preg: rematerialize cheap constants at reload sites.
     */
    if (!mach_rex(e, true, false, false, preg >= 8) ||
        !mach_u8(e, (unsigned)(0xb8 + (preg & 7))) ||
        !mach_i64(e, e->imm_fold[vreg]))
      return false;
  } else if (e->addr_fold && e->addr_fold_known &&
             e->addr_fold_known[vreg]) {
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
    if (!mach_rex(e, true, preg >= 8, false, false) ||
        !mach_u8(e, 0x8b) || !mach_u8(e, modrm) || !mach_i32(e, off))
      return false;
  }
  if (e->color_seeded)
    e->color_seeded[vreg] = true;
  mach_rax_invalidate(e);
  return true;
}

static bool mach_push_callee_saves(ny_x64_mach_enc_t *e, unsigned mask) {
  /*
   * order: r12,r13,r14,r15,rbx
   */
  static const unsigned char ops[5][3] = {
      {2, 0x41, 0x54}, {2, 0x41, 0x55}, {2, 0x41, 0x56}, {2, 0x41, 0x57},
      {1, 0x53, 0x00}, /* push %rbx */
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
static bool mach_pop_callee_saves(ny_x64_mach_enc_t *e, unsigned mask) {
  /*
   * reverse: rbx, r15..r12
   */
  static const unsigned char ops[5][3] = {
      {2, 0x41, 0x5c}, {2, 0x41, 0x5d}, {2, 0x41, 0x5e}, {2, 0x41, 0x5f},
      {1, 0x5b, 0x00}, /* pop %rbx */
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
static bool mach_store_rax(ny_x64_mach_enc_t *e, int off);
static bool mach_mov_reg_rax(ny_x64_mach_enc_t *e, int preg);

static bool mach_store_preg_home(ny_x64_mach_enc_t *e, int preg, int off) {
  /*
   * movq %preg, off(%rbp).
   */
  unsigned char modrm = (unsigned char)(0x85 | ((preg & 7) << 3));
  return mach_rex(e, true, preg >= 8, false, false) && mach_u8(e, 0x89) &&
         mach_u8(e, modrm) && mach_i32(e, off);
}

static bool mach_save_live_out(ny_x64_mach_enc_t *e, size_t block) {
  if (!e || !e->mach || block >= e->mach->block_len)
    return true;
  const ny_mach_block_t *b = &e->mach->blocks[block];
  if (!b->inst_count)
    return true;
  size_t last = b->first_inst + b->inst_count - 1;
  for (uint32_t v = 0; v < e->colors_len; ++v) {
    const ny_mach_live_segment_t *seg =
        mach_segment_at(e, v, last);
    int preg = mach_vreg_preg(e, v);
    /*
     * A carried segment has an allocator-verified physical-register contract
     * on every incoming loop edge. Ordinary live-outs still use canonical
     * homes because their successor may have unrelated predecessors.
     */
    if (!seg || preg < 0 || !e->color_seeded || !e->color_seeded[v])
      continue;
    if (seg->carried)
      continue;
    if (!mach_store_preg_home(e, preg, mach_vreg_home_off(e, v)))
      return false;
    e->color_seeded[v] = false;
    mach_rax_invalidate(e);
  }
  return true;
}

static bool mach_commit_vreg(ny_x64_mach_enc_t *e, const ny_mach_operand_t *dst,
                            int off) {
  if (dst && dst->kind == NY_MACH_OPERAND_VREG) {
    int preg = mach_vreg_preg(e, dst->as.reg);
    if (preg >= 0) {
      if (!mach_mov_reg_rax(e, preg))
        return false;
      if (mach_vreg_future_use_requires_home(e, dst->as.reg)) {
        if (!mach_store_rax(e, off))
          return false;
      } else {
        /*
         * The result lives only in its colored register.  `%rax` contains it
         * transiently, not the old source's stack home; retaining the prior
         * sticky-home tag can make a later load reuse unrelated arithmetic.
         */
        mach_rax_invalidate(e);
      }
      if (e->color_seeded && dst->as.reg < e->colors_len)
        e->color_seeded[dst->as.reg] = true;
      return true;
    }
  }
  if (dst && dst->kind == NY_MACH_OPERAND_VREG && !e->no_sticky &&
      mach_is_i64(e->mach, dst) &&
      mach_vreg_can_forward_to_next_rax(e, dst->as.reg)) {
    e->pending_rax_off = off;
    e->pending_rax_valid = true;
    return true;
  }
  return mach_store_rax(e, off);
}

static bool mach_mov_reg_rax(ny_x64_mach_enc_t *e, int preg) {
  /*
   * movq %rax, %preg.
   */
  return mach_rex(e, true, false, false, preg >= 8) && mach_u8(e, 0x89) &&
         mach_u8(e, (unsigned char)(0xc0 | (preg & 7)));
}

static bool mach_mov_preg_preg(ny_x64_mach_enc_t *e, int dst, int src) {
  if (dst == src)
    return true;
  return mach_rex(e, true, src >= 8, false, dst >= 8) &&
         mach_u8(e, 0x89) &&
         mach_u8(e, (unsigned char)(0xc0 | ((src & 7) << 3) | (dst & 7)));
}

static bool mach_alu_preg_imm(ny_x64_mach_enc_t *e, ny_mach_opcode_t op,
                              int dst, int64_t imm) {
  if ((int64_t)(int32_t)imm != imm)
    return false;
  if (op == NY_MACH_MUL) {
    return mach_rex(e, true, dst >= 8, false, dst >= 8) &&
           mach_u8(e, 0x69) &&
           mach_u8(e, (unsigned char)(0xc0 | ((dst & 7) << 3) | (dst & 7))) &&
           mach_i32(e, (int32_t)imm);
  }
  unsigned ext = op == NY_MACH_ADD ? 0
                 : op == NY_MACH_SUB ? 5
                 : op == NY_MACH_AND ? 4
                 : op == NY_MACH_OR  ? 1
                 : op == NY_MACH_XOR ? 6
                                      : 0xff;
  if (ext == 0xff)
    return false;
  return mach_rex(e, true, false, false, dst >= 8) &&
         mach_u8(e, 0x81) &&
         mach_u8(e, (unsigned char)(0xc0 | (ext << 3) | (dst & 7))) &&
         mach_i32(e, (int32_t)imm);
}

static bool mach_alu_preg_preg(ny_x64_mach_enc_t *e, ny_mach_opcode_t op,
                               int dst, int src) {
  unsigned char opc = op == NY_MACH_ADD ? 0x03
                      : op == NY_MACH_SUB ? 0x2b
                      : op == NY_MACH_AND ? 0x23
                      : op == NY_MACH_OR  ? 0x0b
                      : op == NY_MACH_XOR ? 0x33
                                           : 0;
  if (op == NY_MACH_MUL) {
    return mach_rex(e, true, dst >= 8, false, src >= 8) &&
           mach_u8(e, 0x0f) && mach_u8(e, 0xaf) &&
           mach_u8(e, (unsigned char)(0xc0 | ((dst & 7) << 3) | (src & 7)));
  }
  if (!opc)
    return false;
  return mach_rex(e, true, dst >= 8, false, src >= 8) &&
         mach_u8(e, opc) &&
         mach_u8(e, (unsigned char)(0xc0 | ((dst & 7) << 3) | (src & 7)));
}

static bool mach_pow2_divisor(const ny_mach_operand_t *op,
                              const int64_t *imm_fold,
                              const bool *imm_fold_known,
                              size_t vreg_len, unsigned *shift_out) {
  int64_t divisor = 0;
  if (!op || !shift_out)
    return false;
  if (op->kind == NY_MACH_OPERAND_IMM) {
    divisor = op->as.imm;
  } else if (op->kind == NY_MACH_OPERAND_VREG && imm_fold &&
             imm_fold_known && op->as.reg < vreg_len &&
             imm_fold_known[op->as.reg]) {
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

/*
 * Granlund-Montgomery signed division magic for a positive divisor ad > 1
 * (Hacker's Delight 10-3, 64-bit form).  Returns the magic multiplier M (a
 * signed 64-bit value; negative means the top bit is set) and post-shift s
 * such that, for every int64 x:
 *   q = mulhs(x, M); if (M < 0) q += x; q >>= s; q -= (x >> 63);
 * equals x / ad (C truncation toward zero).  Verified against __int128
 * division across every divisor in [-4096,4096] and the full numerator range.
 */
static void mach_sdiv_magic(uint64_t ad, int64_t *magic_out,
                            unsigned *shift_out) {
  const uint64_t two63 = (uint64_t)1 << 63;
  uint64_t anc = two63 - 1 - (two63 % ad);
  unsigned p = 63;
  uint64_t q1 = two63 / anc;
  uint64_t r1 = two63 - q1 * anc;
  uint64_t q2 = two63 / ad;
  uint64_t r2 = two63 - q2 * ad;
  uint64_t delta;
  do {
    p++;
    q1 <<= 1;
    r1 <<= 1;
    if (r1 >= anc) { q1++; r1 -= anc; }
    q2 <<= 1;
    r2 <<= 1;
    if (r2 >= ad)  { q2++; r2 -= ad; }
    delta = ad - r2;
  } while (q1 < delta || (q1 == delta && r1 == 0));
  *magic_out = (int64_t)(q2 + 1);
  *shift_out = p - 64;
}
static bool mach_mov_rax_reg(ny_x64_mach_enc_t *e, int preg) {
  /*
   * movq %preg, %rax — REX.B extends the source register.
   */
  if (!(mach_rex(e, true, false, false, preg >= 8) && mach_u8(e, 0x8b) &&
        mach_u8(e, (unsigned char)(0xc0 | (preg & 7)))))
    return false;
  mach_rax_invalidate(e);
  return true;
}

static bool mach_mov_rax_imm(ny_x64_mach_enc_t *e, int64_t v) {
  mach_rax_invalidate(e);
  if (!(mach_rex(e, true, false, false, false) && mach_u8(e, 0xb8) && mach_i64(e, v)))
    return false;
  /*
   * Immediate not tied to a slot until store.
   */
  return true;
}

static void mach_rax_invalidate(ny_x64_mach_enc_t *e) {
  e->last_rax_valid = false;
  e->last_rax_off = 0;
}

static bool mach_load_rax(ny_x64_mach_enc_t *e, int off) {
  /*
   * Skip reload when %rax already holds this slot (QBE-style copy elision).
   */
  if (!e->no_sticky && e->last_rax_valid && e->last_rax_off == off)
    return true;
  if (!(mach_rex(e, true, false, false, false) && mach_u8(e, 0x8b) && mach_u8(e, 0x85) &&
        mach_i32(e, off)))
    return false;
  e->last_rax_valid = true;
  e->last_rax_off = off;
  return true;
}

static bool mach_store_rax(ny_x64_mach_enc_t *e, int off) {
  if (!(mach_rex(e, true, false, false, false) && mach_u8(e, 0x89) && mach_u8(e, 0x85) &&
        mach_i32(e, off)))
    return false;
  e->last_rax_valid = true;
  e->last_rax_off = off;
  return true;
}

static bool mach_load_rcx(ny_x64_mach_enc_t *e, int off) {
  return mach_rex(e, true, false, false, false) && mach_u8(e, 0x8b) && mach_u8(e, 0x8d) &&
         mach_i32(e, off);
}
static bool mach_load_ptr_rcx(ny_x64_mach_enc_t *e,
                              const ny_mach_operand_t *op) {
  if (!e || !op)
    return false;
  if (op->kind == NY_MACH_OPERAND_VREG) {
    if (!mach_seed_colored_preg(e, op->as.reg))
      return false;
    int preg = mach_vreg_preg(e, op->as.reg);
    if (preg >= 0)
      return mach_mov_preg_preg(e, 1, preg);
    if (!mach_load_rax(e, mach_slot_off(e, op)))
      return false;
    return mach_mov_preg_preg(e, 1, 0);
  }
  if (op->kind == NY_MACH_OPERAND_FRAME)
    return mach_load_rcx(e, mach_slot_off(e, op));
  return false;
}


static bool mach_load_op_rax(ny_x64_mach_enc_t *e, const ny_mach_operand_t *op) {
  if (op->kind == NY_MACH_OPERAND_IMM)
    return mach_mov_rax_imm(e, op->as.imm);
  if (op->kind == NY_MACH_OPERAND_VREG) {
    if (e->imm_fold && e->imm_fold_known && op->as.reg < e->mach->vreg_len &&
        e->imm_fold_known[op->as.reg])
      return mach_mov_rax_imm(e, e->imm_fold[op->as.reg]);
    if (e->addr_fold && e->addr_fold_known && op->as.reg < e->mach->vreg_len &&
        e->addr_fold_known[op->as.reg]) {
      mach_rax_invalidate(e);
      return mach_remat_addr_preg(e, 0, &e->addr_fold[op->as.reg]);
    }
    if (!mach_seed_colored_preg(e, op->as.reg))
      return false;
    int preg = mach_vreg_preg(e, op->as.reg);
    if (preg >= 0) {
      if (!mach_mov_rax_reg(e, preg))
        return false;
      /*
       * Dual-write keeps stack == preg; mark sticky to this home.
       */
      e->last_rax_valid = true;
      e->last_rax_off = mach_slot_off(e, op);
      return true;
    }
  }
  return mach_load_rax(e, mach_slot_off(e, op));
}

static bool mach_add_patch(ny_x64_mach_enc_t *e, uint32_t block) {
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

static bool mach_add_reloc(ny_x64_mach_enc_t *e, const char *sym, int type) {
  if (e->reloc_count == e->reloc_cap) {
    size_t nc = e->reloc_cap ? e->reloc_cap * 2 : 16;
    ny_x64_obj_reloc_t *nr = realloc(e->relocs, nc * sizeof(*nr));
    if (!nr)
      return mach_err(e, "x64 machine form encode: reloc OOM");
    e->relocs = nr;
    e->reloc_cap = nc;
  }
  ny_x64_obj_reloc_t *r = &e->relocs[e->reloc_count++];
  memset(r, 0, sizeof(*r));
  snprintf(r->symbol, sizeof(r->symbol), "%s", sym ? sym : "");
  r->disp_off = e->code.len;
  r->type = type;
  return true;
}

static bool mach_is_i64(const ny_mach_func_t *m, const ny_mach_operand_t *op) {
  return op && op->kind == NY_MACH_OPERAND_VREG && op->as.reg < m->vreg_len &&
         (m->vreg_types[op->as.reg] == NY_MACH_TYPE_I64 ||
          m->vreg_types[op->as.reg] == NY_MACH_TYPE_PTR);
}

static bool mach_is_f64(const ny_mach_func_t *m, const ny_mach_operand_t *op) {
  return op && op->kind == NY_MACH_OPERAND_VREG && op->as.reg < m->vreg_len &&
         m->vreg_types[op->as.reg] == NY_MACH_TYPE_F64;
}

static bool mach_is_f32(const ny_mach_func_t *m, const ny_mach_operand_t *op) {
  return op && op->kind == NY_MACH_OPERAND_VREG && op->as.reg < m->vreg_len &&
         m->vreg_types[op->as.reg] == NY_MACH_TYPE_F32;
}

static bool mach_is_float(const ny_mach_func_t *m, const ny_mach_operand_t *op) {
  return mach_is_f64(m, op) || mach_is_f32(m, op);
}

static bool mach_is_v128(const ny_mach_func_t *m, const ny_mach_operand_t *op) {
  if (!op || op->kind != NY_MACH_OPERAND_VREG || op->as.reg >= m->vreg_len ||
      !m->vreg_types)
    return false;
  ny_mach_type_t t = m->vreg_types[op->as.reg];
  return t == NY_MACH_TYPE_V128_I64 || t == NY_MACH_TYPE_V128_F64 ||
         t == NY_MACH_TYPE_V128_F32 || t == NY_MACH_TYPE_V256_I64;
}

static ny_mach_type_t mach_v128_type(const ny_mach_func_t *m,
                                     const ny_mach_operand_t *op) {
  if (!mach_is_v128(m, op))
    return NY_MACH_TYPE_NONE;
  return m->vreg_types[op->as.reg];
}

static bool mach_op_is_vreg(const ny_mach_operand_t *op, uint32_t vreg) {
  return op && op->kind == NY_MACH_OPERAND_VREG && op->as.reg == vreg;
}

static bool mach_use_requires_home(const ny_mach_func_t *m,
                                   const ny_mach_inst_t *in,
                                   uint32_t vreg) {
  if (!m || !in)
    return false;
  if (mach_op_is_vreg(&in->src1, vreg) || mach_op_is_vreg(&in->src2, vreg))
    return true;
  if (mach_op_is_vreg(&in->dst, vreg) &&
      in->opcode == NY_MACH_STORE)
    return true;
  if (mach_op_is_vreg(&in->src0, vreg)) {
    switch (in->opcode) {
    case NY_MACH_COPY:
    case NY_MACH_ADD:
    case NY_MACH_SUB:
    case NY_MACH_MUL:
    case NY_MACH_DIV:
    case NY_MACH_MOD:
    case NY_MACH_SQRT:
    case NY_MACH_SIN:
    case NY_MACH_COS:
    case NY_MACH_AND:
    case NY_MACH_OR:
    case NY_MACH_XOR:
    case NY_MACH_SHL:
    case NY_MACH_SAR:
    case NY_MACH_CMP:
    case NY_MACH_STORE:
    case NY_MACH_RET:
      return !mach_is_i64(m, &in->src0);
    default:
      return true;
    }
  }
  if (in->opcode == NY_MACH_CALL && in->args) {
    for (size_t i = 0; i < in->args_len; ++i)
      if (mach_op_is_vreg(&in->args[i], vreg))
        return true;
  }
  return false;
}
static bool mach_inst_uses_vreg(const ny_mach_inst_t *in, uint32_t vreg) {
  if (!in)
    return false;
  if (mach_op_is_vreg(&in->src0, vreg) ||
      mach_op_is_vreg(&in->src1, vreg) ||
      mach_op_is_vreg(&in->src2, vreg) ||
      (in->opcode == NY_MACH_STORE && mach_op_is_vreg(&in->dst, vreg)))
    return true;
  for (size_t i = 0; in->opcode == NY_MACH_CALL && i < in->args_len; ++i)
    if (mach_op_is_vreg(&in->args[i], vreg))
      return true;
  return false;
}

/*
 * A spilled i64 result may skip its canonical home only when the immediately
 * following instruction consumes it as src0 and no later instruction refers
 * to the vreg.  The encoder publishes the RAX slot tag after segment-end
 * bookkeeping so an unrelated home save cannot invalidate it.
 */
static bool mach_vreg_can_forward_to_next_rax(const ny_x64_mach_enc_t *e,
                                              uint32_t vreg) {
  if (!e || !e->mach || e->cur_inst + 1 >= e->mach->inst_len)
    return false;
  size_t block = mach_block_for_inst(e, e->cur_inst);
  if (block == SIZE_MAX)
    return false;
  const ny_mach_block_t *b = &e->mach->blocks[block];
  size_t next_index = e->cur_inst + 1;
  if (next_index >= b->first_inst + b->inst_count)
    return false;
  const ny_mach_inst_t *next = &e->mach->insts[next_index];
  if (!mach_op_is_vreg(&next->src0, vreg) ||
      mach_use_requires_home(e->mach, next, vreg))
    return false;
  for (size_t i = next_index + 1; i < e->mach->inst_len; ++i)
    if (mach_inst_uses_vreg(&e->mach->insts[i], vreg))
      return false;
  return true;
}

static int mach_carried_vreg_preg(const ny_x64_mach_enc_t *e, uint32_t vreg) {
  if (!e)
    return -1;
  for (size_t i = 0; i < e->regalloc.segment_len; ++i) {
    const ny_mach_live_segment_t *seg = &e->regalloc.segments[i];
    if (seg->vreg == vreg && seg->carried && seg->color >= 0 &&
        seg->color < MIR_COLOR_N)
      return mach_color_preg[seg->color];
  }
  return -1;
}

/*
 * A carried phi intentionally has no home write on its backedge.  When the
 * carried value later leaves the loop and is consumed by an ordinary segment,
 * materialize its current preg once on that cold path before the consumer.
 */
static bool mach_sync_carried_consumers(ny_x64_mach_enc_t *e,
                                        const ny_mach_block_t *block) {
  if (!e || !e->mach || !block || !e->color_seeded)
    return true;
  for (uint32_t v = 0; v < e->colors_len; ++v) {
    const ny_mach_live_segment_t *seg =
        mach_segment_at(e, v, block->first_inst);
    if (seg && seg->carried)
      continue;
    bool used = false;
    for (size_t i = 0; i < block->inst_count; ++i)
      if (mach_inst_uses_vreg(&e->mach->insts[block->first_inst + i], v)) {
        used = true;
        break;
      }
    if (!used)
      continue;
    int preg = mach_carried_vreg_preg(e, v);
    if (preg < 0)
      continue;
    if (!mach_store_preg_home(e, preg, mach_vreg_home_off(e, v)))
      return false;
    e->color_seeded[v] = false;
    mach_rax_invalidate(e);
  }
  return true;
}

static bool mach_vreg_future_use_requires_home(
    const ny_x64_mach_enc_t *e, uint32_t vreg) {
  if (!e || !e->mach)
    return true;
  /*
   * Call lowering reloads uncolored operands from canonical homes. Keep this
   * ABI use visible even when the allocator's live segment ends before the
   * call because the call argument list is not a normal src operand.
   */
  for (size_t i = e->cur_inst + 1; i < e->mach->inst_len; ++i) {
    const ny_mach_inst_t *in = &e->mach->insts[i];
    if (in->opcode != NY_MACH_CALL || !in->args)
      continue;
    for (size_t a = 0; a < in->args_len; ++a)
      if (mach_op_is_vreg(&in->args[a], vreg))
        return true;
  }
  const ny_mach_live_segment_t *seg =

      mach_segment_at(e, vreg, e->cur_inst);
  if (!seg || seg->end <= e->cur_inst)
    return false;
  if (seg->spill)
    return true;
  size_t end = seg->end;
  if (end >= e->mach->inst_len)
    end = e->mach->inst_len - 1;
  for (size_t i = e->cur_inst + 1; i <= end; ++i)
    if (mach_use_requires_home(e->mach, &e->mach->insts[i], vreg))
      return true;
  return false;
}

/*
 * movdqu xmmN <-> mem: F3 REX 0F 6F/7F 85 disp32 (unaligned).
 */
static bool mach_load_xmm_v128(ny_x64_mach_enc_t *e, int off, unsigned reg) {
  if (reg >= 16)
    return mach_err(e, "x64 machine form encode: unsupported XMM register");
  return mach_u8(e, 0xf3) &&
         (reg >= 8 ? mach_rex(e, false, true, false, false) : true) &&
         mach_u8(e, 0x0f) && mach_u8(e, 0x6f) &&
         mach_u8(e, 0x85 | ((reg & 7) << 3)) && mach_i32(e, off);
}

static bool mach_store_xmm_v128(ny_x64_mach_enc_t *e, int off, unsigned reg) {
  if (reg >= 16)
    return mach_err(e, "x64 machine form encode: unsupported XMM register");
  return mach_u8(e, 0xf3) &&
         (reg >= 8 ? mach_rex(e, false, true, false, false) : true) &&
         mach_u8(e, 0x0f) && mach_u8(e, 0x7f) &&
         mach_u8(e, 0x85 | ((reg & 7) << 3)) && mach_i32(e, off);
}

static bool mach_load_xmm0_v128(ny_x64_mach_enc_t *e, int off) {
  return mach_load_xmm_v128(e, off, 0);
}

static bool mach_store_xmm0_v128(ny_x64_mach_enc_t *e, int off) {
  return mach_store_xmm_v128(e, off, 0);
}

/*
 * ── ymm (256-bit VEX) helpers for V256_I64 fast path ───────────────
 */

/*
 * vmovdqu ymmN, [rbp+off] — unaligned stack homes need no 32-byte rbp alignment.
 */
static bool mach_ymm_load_v256(ny_x64_mach_enc_t *e, int off, unsigned reg) {
  if (reg >= 16)
    return mach_err(e, "x64: unsupported YMM register");
  return mach_u8(e, 0xc4) && mach_u8(e, 0xe1) && mach_u8(e, 0x7e) &&
         mach_u8(e, 0x6f) && mach_u8(e, 0x85 | ((reg & 7) << 3)) &&
         mach_i32(e, off);
}

/*
 * vmovdqu [rbp+off], ymmN — unaligned stack homes need no 32-byte rbp alignment.
 */
static bool mach_ymm_store_v256(ny_x64_mach_enc_t *e, int off, unsigned reg) {
  if (reg >= 16)
    return mach_err(e, "x64: unsupported YMM register");
  return mach_u8(e, 0xc4) && mach_u8(e, 0xe1) && mach_u8(e, 0x7e) &&
         mach_u8(e, 0x7f) && mach_u8(e, 0x85 | ((reg & 7) << 3)) &&
         mach_i32(e, off);
}

/*
 * vmovdqa ymmN, [rax] — VEX.256.66.0F 6F /r (no disp)
 */
static bool mach_ymm_load_v256_ptr(ny_x64_mach_enc_t *e, unsigned reg) {
  if (reg >= 16)
    return mach_err(e, "x64: unsupported YMM register");
  return mach_u8(e, 0xc4) && mach_u8(e, 0xe1) && mach_u8(e, 0x7e) &&
         mach_u8(e, 0x6f) && mach_u8(e, (reg & 7) << 3);
}

/*
 * vmovdqa [rax], ymmN — VEX.256.66.0F 7F /r (no disp)
 */
static bool mach_ymm_store_v256_ptr(ny_x64_mach_enc_t *e, unsigned reg) {
  if (reg >= 16)
    return mach_err(e, "x64: unsupported YMM register");
  return mach_u8(e, 0xc4) && mach_u8(e, 0xe1) && mach_u8(e, 0x7e) &&
         mach_u8(e, 0x7f) && mach_u8(e, (reg & 7) << 3);
}

/*
 * vmovdqa ymmD, ymmS — VEX.256.66.0F 6F /r
 */
static bool mach_ymm_mov_reg(ny_x64_mach_enc_t *e, unsigned dst,
                             unsigned src) {
  if (dst >= 16 || src >= 16)
    return mach_err(e, "x64: unsupported YMM register");
  return mach_u8(e, 0xc4) && mach_u8(e, 0xe1) && mach_u8(e, 0x7d) &&
         mach_u8(e, 0x6f) &&
         mach_u8(e, 0xc0 | ((dst & 7) << 3) | (src & 7));
}

/*
 * vp* ymmD, ymmD, ymmS — VEX.256.66.0F.W1 opc /r
 */
static bool mach_ymm_op_reg(ny_x64_mach_enc_t *e, unsigned dst, unsigned src,
                            unsigned opc) {
  if (dst >= 16 || src >= 16)
    return mach_err(e, "x64: unsupported YMM register");
  return mach_u8(e, 0xc4) && mach_u8(e, 0xe1) && mach_u8(e, 0x7d) &&
         mach_u8(e, opc) &&
         mach_u8(e, 0xc0 | ((dst & 7) << 3) | (src & 7));
}

/*
 * vp* ymmD, ymmD, [rbp+off] — VEX.256.66.0F.W1 opc 85 disp32
 */
static bool mach_ymm_op_mem_reg(ny_x64_mach_enc_t *e, unsigned dst, int off,
                                unsigned opc) {
  if (dst >= 16)
    return mach_err(e, "x64: unsupported YMM register");
  return mach_u8(e, 0xc4) && mach_u8(e, 0xe1) && mach_u8(e, 0x7d) &&
         mach_u8(e, opc) &&
         mach_u8(e, 0x85 | ((dst & 7) << 3)) && mach_i32(e, off);
}

static bool mach_load_xmm_v128_ptr(ny_x64_mach_enc_t *e, unsigned reg) {
  if (reg >= 16)
    return mach_err(e, "x64 machine form encode: unsupported XMM register");
  return mach_u8(e, 0xf3) &&
         (reg >= 8 ? mach_rex(e, false, true, false, false) : true) &&
         mach_u8(e, 0x0f) && mach_u8(e, 0x6f) &&
         mach_u8(e, (unsigned)((reg & 7) << 3));
}

static bool mach_store_xmm_v128_ptr(ny_x64_mach_enc_t *e, unsigned reg) {
  if (reg >= 16)
    return mach_err(e, "x64 machine form encode: unsupported XMM register");
  return mach_u8(e, 0xf3) &&
         (reg >= 8 ? mach_rex(e, false, true, false, false) : true) &&
         mach_u8(e, 0x0f) && mach_u8(e, 0x7f) &&
         mach_u8(e, (unsigned)((reg & 7) << 3));
}

static bool mach_vec_mov_reg(ny_x64_mach_enc_t *e, unsigned dst,
                             unsigned src) {
  if (dst >= 16 || src >= 16)
    return mach_err(e, "x64 machine form encode: unsupported XMM register");
  unsigned rex = 0x40 | (dst >= 8 ? 0x04 : 0) | (src >= 8 ? 0x01 : 0);
  return mach_u8(e, 0xf3) && (rex != 0x40 ? mach_u8(e, rex) : true) &&
         mach_u8(e, 0x0f) && mach_u8(e, 0x6f) &&
         mach_u8(e, 0xc0 | ((dst & 7) << 3) | (src & 7));
}

static bool mach_vec_op_reg(ny_x64_mach_enc_t *e, unsigned dst, unsigned src,
                            unsigned prefix, unsigned opc) {
  if (dst >= 16 || src >= 16)
    return mach_err(e, "x64 machine form encode: unsupported XMM register");
  unsigned rex = 0x40 | (dst >= 8 ? 0x04 : 0) | (src >= 8 ? 0x01 : 0);
  return (prefix ? mach_u8(e, prefix) : true) &&
         (rex != 0x40 ? mach_u8(e, rex) : true) && mach_u8(e, 0x0f) &&
         mach_u8(e, opc) && mach_u8(e, 0xc0 | ((dst & 7) << 3) | (src & 7));
}

static bool mach_vec_op_mem_reg(ny_x64_mach_enc_t *e, unsigned dst, int off,
                                unsigned prefix, unsigned opc) {
  if (dst >= 16)
    return mach_err(e, "x64 machine form encode: unsupported XMM register");
  return (prefix ? mach_u8(e, prefix) : true) &&
         (dst >= 8 ? mach_rex(e, false, true, false, false) : true) && mach_u8(e, 0x0f) &&
         mach_u8(e, opc) && mach_u8(e, 0x85 | ((dst & 7) << 3)) &&
         mach_i32(e, off);
}

/*
 * movsd/movss xmmN <-> mem: F2/F3 REX 0F 10/11 modrm disp32.
 */
static bool mach_load_xmm(ny_x64_mach_enc_t *e, int off, bool f32,
                          unsigned reg) {
  if (reg >= 16)
    return mach_err(e, "x64 machine form encode: unsupported XMM register");
  return mach_u8(e, f32 ? 0xf3 : 0xf2) &&
         (reg >= 8 ? mach_rex(e, false, true, false, false) : true) &&
         mach_u8(e, 0x0f) && mach_u8(e, 0x10) &&
         mach_u8(e, 0x85 | ((reg & 7) << 3)) &&
         mach_i32(e, off);
}

static bool mach_store_xmm(ny_x64_mach_enc_t *e, int off, bool f32,
                           unsigned reg) {
  if (reg >= 16)
    return mach_err(e, "x64 machine form encode: unsupported XMM register");
  return mach_u8(e, f32 ? 0xf3 : 0xf2) &&
         (reg >= 8 ? mach_rex(e, false, true, false, false) : true) &&
         mach_u8(e, 0x0f) && mach_u8(e, 0x11) &&
         mach_u8(e, 0x85 | ((reg & 7) << 3)) &&
         mach_i32(e, off);
}

static bool mach_load_xmm0(ny_x64_mach_enc_t *e, int off, bool f32) {
  return mach_load_xmm(e, off, f32, 0);
}

static bool mach_store_xmm0(ny_x64_mach_enc_t *e, int off, bool f32) {
  return mach_store_xmm(e, off, f32, 0);
}

static bool mach_load_xmm1(ny_x64_mach_enc_t *e, int off, bool f32) {
  return mach_load_xmm(e, off, f32, 1);
}

static bool mach_sse_mov_reg(ny_x64_mach_enc_t *e, unsigned dst,
                             unsigned src, bool f32) {
  if (dst >= 16 || src >= 16)
    return mach_err(e, "x64 machine form encode: unsupported XMM register");
  unsigned rex = 0x40 | (dst >= 8 ? 0x04 : 0) | (src >= 8 ? 0x01 : 0);
  return mach_u8(e, f32 ? 0xf3 : 0xf2) &&
         (rex != 0x40 ? mach_u8(e, rex) : true) && mach_u8(e, 0x0f) &&
         mach_u8(e, 0x10) &&
         mach_u8(e, (unsigned)(0xc0 | ((dst & 7) << 3) | (src & 7)));
}

static bool mach_sse_op_reg(ny_x64_mach_enc_t *e, unsigned dst,
                            unsigned src, bool f32, unsigned opc) {
  if (dst >= 16 || src >= 16)
    return mach_err(e, "x64 machine form encode: unsupported XMM register");
  unsigned rex = 0x40 | (dst >= 8 ? 0x04 : 0) | (src >= 8 ? 0x01 : 0);
  return mach_u8(e, f32 ? 0xf3 : 0xf2) &&
         (rex != 0x40 ? mach_u8(e, rex) : true) && mach_u8(e, 0x0f) &&
         mach_u8(e, opc) &&
         mach_u8(e, (unsigned)(0xc0 | ((dst & 7) << 3) | (src & 7)));
}

static bool mach_sse_op_mem_reg(ny_x64_mach_enc_t *e, unsigned dst, int off,
                                bool f32, unsigned opc) {
  if (dst >= 16)
    return mach_err(e, "x64 machine form encode: unsupported XMM register");
  return mach_u8(e, f32 ? 0xf3 : 0xf2) &&
         (dst >= 8 ? mach_rex(e, false, true, false, false) : true) && mach_u8(e, 0x0f) &&
         mach_u8(e, opc) && mach_u8(e, (unsigned)(0x85 | ((dst & 7) << 3))) &&
         mach_i32(e, off);
}

static bool mach_sse_cmp_reg(ny_x64_mach_enc_t *e, unsigned lhs,
                             unsigned rhs, bool f32) {
  if (lhs >= 16 || rhs >= 16)
    return mach_err(e, "x64 machine form encode: unsupported XMM register");
  unsigned rex = 0x40 | (lhs >= 8 ? 0x04 : 0) | (rhs >= 8 ? 0x01 : 0);
  return (f32 ? true : mach_u8(e, 0x66)) &&
         (rex != 0x40 ? mach_u8(e, rex) : true) && mach_u8(e, 0x0f) &&
         mach_u8(e, 0x2e) &&
         mach_u8(e, (unsigned)(0xc0 | ((lhs & 7) << 3) | (rhs & 7)));
}

static bool mach_load_xmm_ptr(ny_x64_mach_enc_t *e, unsigned dst, bool f32) {
  if (dst >= 16)
    return mach_err(e, "x64 machine form encode: unsupported XMM register");
  return mach_u8(e, f32 ? 0xf3 : 0xf2) &&
         (dst >= 8 ? mach_rex(e, false, true, false, false) : true) &&
         mach_u8(e, 0x0f) && mach_u8(e, 0x10) &&
         mach_u8(e, (unsigned)((dst & 7) << 3));
}

static bool mach_store_xmm_ptr(ny_x64_mach_enc_t *e, unsigned src, bool f32) {
  if (src >= 16)
    return mach_err(e, "x64 machine form encode: unsupported XMM register");
  return mach_u8(e, f32 ? 0xf3 : 0xf2) &&
         (src >= 8 ? mach_rex(e, false, true, false, false) : true) &&
         mach_u8(e, 0x0f) && mach_u8(e, 0x11) &&
         mach_u8(e, (unsigned)(0x01 | ((src & 7) << 3)));
}

static bool mach_mov_rax_xmm(ny_x64_mach_enc_t *e, unsigned dst) {
  if (dst >= 16)
    return mach_err(e, "x64 machine form encode: unsupported XMM register");
  return mach_u8(e, 0x66) &&
         mach_rex(e, true, dst >= 8, false, false) &&
         mach_u8(e, 0x0f) && mach_u8(e, 0x6e) &&
         mach_u8(e, (unsigned)(0xc0 | ((dst & 7) << 3)));
}
static bool mach_mov_xmm_rax(ny_x64_mach_enc_t *e, unsigned src) {
  if (src >= 16)
    return mach_err(e, "x64 machine form encode: unsupported XMM register");
  return mach_u8(e, 0x66) && mach_rex(e, true, src >= 8, false, false) &&
         mach_u8(e, 0x0f) && mach_u8(e, 0x7e) &&
         mach_u8(e, (unsigned)(0xc0 | ((src & 7) << 3)));
}

static bool mach_vextract_i128(ny_x64_mach_enc_t *e, unsigned dst,
                               unsigned src, unsigned imm) {
  if (dst >= 16 || src >= 16)
    return mach_err(e, "x64 machine form encode: unsupported YMM register");
  unsigned vex2 = 0xe3 | (src < 8 ? 0x80 : 0) | (dst < 8 ? 0x20 : 0);
  return mach_u8(e, 0xc4) && mach_u8(e, vex2) && mach_u8(e, 0x7d) &&
         mach_u8(e, 0x39) &&
         mach_u8(e, (unsigned)(0xc0 | ((src & 7) << 3) | (dst & 7))) &&
         mach_u8(e, imm & 1);
}

static bool mach_float_begin_segments(ny_x64_mach_enc_t *e) {
  if (!e || !e->fp_fast_path || !e->fp_color_seeded)
    return true;
  for (uint32_t v = 0; v < e->fp_colors_len; ++v) {
    const ny_mach_live_segment_t *seg =
        mach_fsegment_at(e, v, e->cur_inst);
    if (!seg || seg->start != e->cur_inst)
      continue;
    e->fp_color_seeded[v] =
        mach_fsegment_carries(e, v, seg) && e->fp_color_seeded[v];
  }
  return true;
}

static bool mach_float_end_segments(ny_x64_mach_enc_t *e) {
  if (!e || !e->fp_fast_path || !e->fp_color_seeded)
    return true;
  for (uint32_t v = 0; v < e->fp_colors_len; ++v) {
    const ny_mach_live_segment_t *seg =
        mach_fsegment_at(e, v, e->cur_inst);
    if (!seg || seg->end != e->cur_inst)
      continue;
    const ny_mach_live_segment_t *next =
        mach_fsegment_at(e, v, e->cur_inst + 1);
    bool needs_home = seg->spill || (next && !mach_fsegment_carries(e, v, next));
    if (needs_home && seg->color >= 0 && e->fp_color_seeded[v]) {
      if (!mach_store_xmm(e, mach_vreg_home_off(e, v),
                          e->mach->vreg_types[v] == NY_MACH_TYPE_F32,
                          (unsigned)seg->color))
        return false;
    }
    if (seg->spill || !next || !mach_fsegment_carries(e, v, next))
      e->fp_color_seeded[v] = false;
  }
  return true;
}

static const ny_mach_live_segment_t *mach_vsegment_at(
    const ny_x64_mach_enc_t *e, uint32_t vreg, size_t inst) {
  return e ? ny_mach_regalloc_segment_at(&e->vec_regalloc, vreg, inst) : NULL;
}

static bool mach_vsegment_carries(const ny_x64_mach_enc_t *e, uint32_t vreg,
                                  const ny_mach_live_segment_t *seg) {
  if (!e || !seg || seg->start == 0 || seg->reload || seg->color < 0)
    return false;
  const ny_mach_live_segment_t *prev =
      mach_vsegment_at(e, vreg, seg->start - 1);
  /*
   * CFG joins use canonical stack homes. A register can keep the same color
   * across an edge without being resident in the destination block.
   */
  return prev && prev->block == seg->block && prev->end + 1 == seg->start &&
         !prev->spill &&
         prev->color == seg->color;
}

static bool mach_vector_begin_segments(ny_x64_mach_enc_t *e) {
  if (!e || !e->vec_fast_path || !e->vec_color_seeded)
    return true;
  for (uint32_t v = 0; v < e->vec_colors_len; ++v) {
    const ny_mach_live_segment_t *seg = mach_vsegment_at(e, v, e->cur_inst);
    if (!seg || seg->start != e->cur_inst)
      continue;
    e->vec_color_seeded[v] =
        mach_vsegment_carries(e, v, seg) && e->vec_color_seeded[v];
  }
  return true;
}

static bool mach_vector_end_segments(ny_x64_mach_enc_t *e) {
  if (!e || !e->vec_fast_path || !e->vec_color_seeded)
    return true;
  for (uint32_t v = 0; v < e->vec_colors_len; ++v) {
    const ny_mach_live_segment_t *seg = mach_vsegment_at(e, v, e->cur_inst);
    if (!seg || seg->end != e->cur_inst)
      continue;
    const ny_mach_live_segment_t *next =
        mach_vsegment_at(e, v, e->cur_inst + 1);
    bool needs_home = seg->spill || (next && !mach_vsegment_carries(e, v, next));
    if (needs_home && seg->color >= 0 && e->vec_color_seeded[v]) {
      bool is_v256 = e->mach && v < e->mach->vreg_len &&
                     e->mach->vreg_types[v] == NY_MACH_TYPE_V256_I64;
      if (!(is_v256 ? mach_ymm_store_v256(e, mach_vreg_home_off(e, v),
                                          (unsigned)seg->color)
                    : mach_store_xmm_v128(e, mach_vreg_home_off(e, v),
                                          (unsigned)seg->color)))
        return false;
    }
    if (seg->spill || !next || !mach_vsegment_carries(e, v, next))
      e->vec_color_seeded[v] = false;
  }
  return true;
}

static bool mach_seed_vector_preg(ny_x64_mach_enc_t *e, uint32_t vreg) {
  int preg = mach_vreg_vpreg(e, vreg);
  if (preg < 0 || (e->vec_color_seeded && e->vec_color_seeded[vreg]))
    return true;
  bool is_v256 = e->mach && vreg < e->mach->vreg_len &&
                 e->mach->vreg_types[vreg] == NY_MACH_TYPE_V256_I64;
  if (!(is_v256 ? mach_ymm_load_v256(e, mach_vreg_home_off(e, vreg),
                                     (unsigned)preg)
                : mach_load_xmm_v128(e, mach_vreg_home_off(e, vreg),
                                     (unsigned)preg)))
    return false;
  if (e->vec_color_seeded)
    e->vec_color_seeded[vreg] = true;
  return true;
}

static bool mach_load_vector_operand(ny_x64_mach_enc_t *e,
                                     const ny_mach_operand_t *op,
                                     unsigned scratch, unsigned *out_reg) {
  if (op && op->kind == NY_MACH_OPERAND_VREG) {
    int preg = mach_vreg_vpreg(e, op->as.reg);
    if (preg >= 0) {
      if (!mach_seed_vector_preg(e, op->as.reg))
        return false;
      *out_reg = (unsigned)preg;
      return true;
    }
  }
  if (!op || (op->kind != NY_MACH_OPERAND_VREG &&
              op->kind != NY_MACH_OPERAND_FRAME))
    return mach_err(e, "x64 machine form encode: unsupported vector operand");
  *out_reg = scratch;
  /*
   * A spilled V256 vreg homes 32 bytes; loading only 128 bits would
   * truncate the upper half.  FRAME operands carry no type and are only
   * ever V128 in the vector fast path, so they keep the xmm load.
   */
  bool is_v256 = op->kind == NY_MACH_OPERAND_VREG && e->mach &&
                 op->as.reg < e->mach->vreg_len && e->mach->vreg_types &&
                 e->mach->vreg_types[op->as.reg] == NY_MACH_TYPE_V256_I64;
  if (is_v256)
    return mach_ymm_load_v256(e, mach_slot_off(e, op), scratch);
  return mach_load_xmm_v128(e, mach_slot_off(e, op), scratch);
}
static bool mach_load_float_operand(ny_x64_mach_enc_t *e,
                                    const ny_mach_operand_t *op, bool f32,
                                    unsigned scratch, unsigned *out_reg);

static bool mach_reduce_add_i64(ny_x64_mach_enc_t *e,
                                const ny_mach_inst_t *in, int dst_off) {
  if (!mach_load_op_rax(e, &in->src0) || !mach_mov_reg_rax(e, 2))
    return false;

  unsigned v = 14;
  if (!mach_load_vector_operand(e, &in->src1, 15, &v))
    return false;
  bool v256 = in->src1.kind == NY_MACH_OPERAND_VREG && e->mach &&
              in->src1.as.reg < e->mach->vreg_len && e->mach->vreg_types &&
              e->mach->vreg_types[in->src1.as.reg] == NY_MACH_TYPE_V256_I64;
  if (v256) {
    if (!mach_vextract_i128(e, 0, v, 0) ||
        !mach_vextract_i128(e, 1, v, 1) ||
        !mach_vec_op_reg(e, 0, 1, 0x66, 0xd4))
      return false;
  } else if (v != 0 && !mach_vec_mov_reg(e, 0, v)) {
    return false;
  }
  if (!mach_u8(e, 0x66) || !mach_u8(e, 0x0f) || !mach_u8(e, 0x70) ||
      !mach_u8(e, 0xc8) || !mach_u8(e, 0x4e) ||
      !mach_vec_op_reg(e, 0, 1, 0x66, 0xd4) ||
      !mach_mov_xmm_rax(e, 0) ||
      !mach_rex(e, true, false, false, false) || !mach_u8(e, 0x01) ||
      !mach_u8(e, 0xd0) || !mach_commit_vreg(e, &in->dst, dst_off))
    return false;
  return true;
}

static bool mach_reduce_add_f64(ny_x64_mach_enc_t *e,
                                const ny_mach_inst_t *in, int dst_off) {
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

  int assigned = in->dst.kind == NY_MACH_OPERAND_VREG
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

static bool mach_vector_copy(ny_x64_mach_enc_t *e,
                             const ny_mach_operand_t *dst,
                             const ny_mach_operand_t *src, int dst_off) {
  int assigned = dst && dst->kind == NY_MACH_OPERAND_VREG
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

static bool mach_vector_load(ny_x64_mach_enc_t *e,
                             const ny_mach_inst_t *in, int dst_off) {
  int assigned = in->dst.kind == NY_MACH_OPERAND_VREG
                     ? mach_vreg_vpreg(e, in->dst.as.reg)
                     : -1;
  unsigned d = assigned >= 0 ? (unsigned)assigned : 14;
  if (in->src0.kind == NY_MACH_OPERAND_FRAME) {
    if (!mach_load_xmm_v128(e, mach_slot_off(e, &in->src0), d))
      return false;
  } else if (in->src0.kind == NY_MACH_OPERAND_VREG) {
    if (!mach_load_op_rax(e, &in->src0) || !mach_load_xmm_v128_ptr(e, d))
      return false;
  } else {
    return mach_err(e, "x64 machine form encode: vector load from pointer");
  }
  if (assigned >= 0) {
    if (e->vec_color_seeded)
      e->vec_color_seeded[in->dst.as.reg] = true;
    return true;
  }
  return mach_store_xmm_v128(e, dst_off, d);
}

static bool mach_vector_store(ny_x64_mach_enc_t *e,
                              const ny_mach_inst_t *in, int dst_off) {
  unsigned s = 14;
  if (!mach_load_vector_operand(e, &in->src0, 14, &s))
    return false;
  if (in->dst.kind == NY_MACH_OPERAND_FRAME)
    return mach_store_xmm_v128(e, dst_off, s);
  if (in->dst.kind == NY_MACH_OPERAND_VREG) {
    if (!mach_load_op_rax(e, &in->dst) || !mach_store_xmm_v128_ptr(e, s))
      return false;
    return true;
  }
  return mach_err(e, "x64 machine form encode: vector store destination");
}

static bool mach_vector_binop(ny_x64_mach_enc_t *e, const ny_mach_inst_t *in,
                              int dst_off, unsigned prefix, unsigned opc) {
  int assigned = in->dst.kind == NY_MACH_OPERAND_VREG
                     ? mach_vreg_vpreg(e, in->dst.as.reg)
                     : -1;
  unsigned d = assigned >= 0 ? (unsigned)assigned : 14;
  unsigned a = 15;
  if (!mach_load_vector_operand(e, &in->src0, d == 15 ? 14 : 15, &a))
    return false;
  if (a != d && !mach_vec_mov_reg(e, d, a))
    return false;
  if (in->src1.kind == NY_MACH_OPERAND_VREG) {
    int breg = mach_vreg_vpreg(e, in->src1.as.reg);
    if (breg >= 0) {
      if (!mach_seed_vector_preg(e, in->src1.as.reg) ||
          !mach_vec_op_reg(e, d, (unsigned)breg, prefix, opc))
        return false;
    } else if (!mach_vec_op_mem_reg(e, d, mach_slot_off(e, &in->src1),
                                    prefix, opc))
      return false;
  } else if (in->src1.kind == NY_MACH_OPERAND_FRAME) {
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
 * ── V256_I64 ymm fast path wrappers ─────────────────────────────────
 */

/*
 * Copy for V256_I64: load src into a ymm reg, move to dst, spill if needed.
 */
static bool mach_vector_ymm_copy(ny_x64_mach_enc_t *e,
                                  const ny_mach_operand_t *dst,
                                  const ny_mach_operand_t *src, int dst_off) {
  int assigned = dst && dst->kind == NY_MACH_OPERAND_VREG
                     ? mach_vreg_vpreg(e, dst->as.reg)
                     : -1;
  unsigned d = assigned >= 0 ? (unsigned)assigned : 14;
  unsigned s = 14;
  /*
   * Use ymm load for V256 sources from frame/vreg homes.
   */
  if (src && src->kind == NY_MACH_OPERAND_VREG) {
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
  } else if (src && src->kind == NY_MACH_OPERAND_FRAME) {
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
 */
static bool mach_vector_ymm_load(ny_x64_mach_enc_t *e,
                                  const ny_mach_inst_t *in, int dst_off) {
  int assigned = in->dst.kind == NY_MACH_OPERAND_VREG
                     ? mach_vreg_vpreg(e, in->dst.as.reg)
                     : -1;
  unsigned d = assigned >= 0 ? (unsigned)assigned : 14;
  if (in->src0.kind == NY_MACH_OPERAND_FRAME) {
    if (!mach_ymm_load_v256(e, mach_slot_off(e, &in->src0), d))
      return false;
  } else if (in->src0.kind == NY_MACH_OPERAND_VREG) {
    if (!mach_load_op_rax(e, &in->src0) || !mach_ymm_load_v256_ptr(e, d))
      return false;
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
static bool mach_vector_ymm_store(ny_x64_mach_enc_t *e,
                                   const ny_mach_inst_t *in, int dst_off) {
  unsigned s = 14;
  if (in->src0.kind == NY_MACH_OPERAND_VREG) {
    int preg = mach_vreg_vpreg(e, in->src0.as.reg);
    if (preg >= 0) {
      if (!mach_seed_vector_preg(e, in->src0.as.reg))
        return false;
      s = (unsigned)preg;
    } else if (!mach_ymm_load_v256(e, mach_slot_off(e, &in->src0), s))
      return false;
  } else if (in->src0.kind == NY_MACH_OPERAND_FRAME) {
    if (!mach_ymm_load_v256(e, mach_slot_off(e, &in->src0), s))
      return false;
  } else {
    return mach_err(e, "x64: ymm store source");
  }
  if (in->dst.kind == NY_MACH_OPERAND_FRAME)
    return mach_ymm_store_v256(e, dst_off, s);
  if (in->dst.kind == NY_MACH_OPERAND_VREG) {
    if (!mach_load_op_rax(e, &in->dst) || !mach_ymm_store_v256_ptr(e, s))
      return false;
    return true;
  }
  return mach_err(e, "x64: ymm store destination");
}

/*
 * Binary op for V256_I64: vpaddq/vpsubq/vpand/vpor/vpxor ymm
 */
static bool mach_vector_ymm_binop(ny_x64_mach_enc_t *e,
                                   const ny_mach_inst_t *in,
                                   int dst_off, unsigned opc) {
  int assigned = in->dst.kind == NY_MACH_OPERAND_VREG
                     ? mach_vreg_vpreg(e, in->dst.as.reg)
                     : -1;
  unsigned d = assigned >= 0 ? (unsigned)assigned : 14;
  unsigned a = 15;
  /*
   * Load src0 into a ymm reg.
   */
  if (in->src0.kind == NY_MACH_OPERAND_VREG) {
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
  } else if (in->src0.kind == NY_MACH_OPERAND_FRAME) {
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
  if (in->src1.kind == NY_MACH_OPERAND_VREG) {
    int breg = mach_vreg_vpreg(e, in->src1.as.reg);
    if (breg >= 0) {
      if (!mach_seed_vector_preg(e, in->src1.as.reg) ||
          !mach_ymm_op_reg(e, d, (unsigned)breg, opc))
        return false;
    } else if (!mach_ymm_op_mem_reg(e, d, mach_slot_off(e, &in->src1), opc))
      return false;
  } else if (in->src1.kind == NY_MACH_OPERAND_FRAME) {
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

static bool mach_vector_shift(ny_x64_mach_enc_t *e, const ny_mach_inst_t *in,
                              int dst_off, unsigned prefix, unsigned opc) {
  int assigned = in->dst.kind == NY_MACH_OPERAND_VREG
                     ? mach_vreg_vpreg(e, in->dst.as.reg)
                     : -1;
  unsigned d = assigned >= 0 ? (unsigned)assigned : 14;
  unsigned a = d == 14 ? 15 : 14;
  if (!mach_load_vector_operand(e, &in->src0, a, &a))
    return false;
  if (a != d && !mach_vec_mov_reg(e, d, a))
    return false;
  if (in->src1.kind == NY_MACH_OPERAND_VREG) {
    int breg = mach_vreg_vpreg(e, in->src1.as.reg);
    if (breg >= 0) {
      if (!mach_seed_vector_preg(e, in->src1.as.reg) ||
          !mach_vec_op_reg(e, d, (unsigned)breg, prefix, opc))
        return false;
    } else if (!mach_vec_op_mem_reg(e, d, mach_slot_off(e, &in->src1),
                                    prefix, opc))
      return false;
  } else if (in->src1.kind == NY_MACH_OPERAND_FRAME) {
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

static bool mach_vector_fma(ny_x64_mach_enc_t *e, const ny_mach_inst_t *in,
                            int dst_off, unsigned prefix, unsigned mul_op,
                            unsigned add_op) {
  if (!mach_vector_binop(e, in, dst_off, prefix, mul_op))
    return false;
  int assigned = in->dst.kind == NY_MACH_OPERAND_VREG
                     ? mach_vreg_vpreg(e, in->dst.as.reg)
                     : -1;
  unsigned d = assigned >= 0 ? (unsigned)assigned : 14;
  const ny_mach_operand_t *src2 = &in->src2;
  if (src2->kind == NY_MACH_OPERAND_VREG) {
    int creg = mach_vreg_vpreg(e, src2->as.reg);
    if (creg >= 0) {
      if (!mach_seed_vector_preg(e, src2->as.reg) ||
          !mach_vec_op_reg(e, d, (unsigned)creg, prefix, add_op))
        return false;
    } else if (!mach_vec_op_mem_reg(e, d, mach_slot_off(e, src2), prefix, add_op))
      return false;
  } else if (src2->kind == NY_MACH_OPERAND_FRAME) {
    if (!mach_vec_op_mem_reg(e, d, mach_slot_off(e, src2), prefix, add_op))
      return false;
  } else
    return mach_err(e, "x64 machine form encode: unsupported vector FMA rhs");
  if (assigned >= 0 && e->vec_color_seeded)
    e->vec_color_seeded[in->dst.as.reg] = true;
  return assigned >= 0 || mach_store_xmm_v128(e, dst_off, d);
}

static bool mach_vector_fast_eligible(const ny_mach_func_t *mach) {
  if (!mach)
    return false;
  bool has_vector = false;
  for (size_t i = 0; i < mach->vreg_len; ++i) {
    ny_mach_type_t t = mach->vreg_types[i];
    if (t == NY_MACH_TYPE_F32 || t == NY_MACH_TYPE_F64)
      return false; /* scalar and vector XMM allocators must never overlap */
    if (t == NY_MACH_TYPE_V128_I64 || t == NY_MACH_TYPE_V128_F64 ||
        t == NY_MACH_TYPE_V128_F32 || t == NY_MACH_TYPE_V256_I64)
      has_vector = true;
  }
  if (!has_vector)
    return false;
  for (size_t i = 0; i < mach->inst_len; ++i) {
    const ny_mach_inst_t *in = &mach->insts[i];
    if (in->opcode == NY_MACH_CALL) {
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
    const ny_mach_operand_t *ops[] = {&in->dst, &in->src0, &in->src1,
                                      &in->src2};
    bool vector_inst = false;
    for (size_t k = 0; k < sizeof(ops) / sizeof(ops[0]); ++k)
      if (mach_is_v128(mach, ops[k]))
        vector_inst = true;
    if (!vector_inst)
      continue;
    if (in->opcode == NY_MACH_RET)
      return false;
    if (in->opcode == NY_MACH_DIV &&
        mach_v128_type(mach, &in->dst) == NY_MACH_TYPE_V128_I64)
      return false;
    if (in->opcode == NY_MACH_COPY && in->src0.kind != NY_MACH_OPERAND_VREG &&
        in->src0.kind != NY_MACH_OPERAND_FRAME)
      return false;
  }
  return true;
}

static bool mach_save_float_live_out(ny_x64_mach_enc_t *e, size_t block) {
  if (!e || !e->fp_fast_path || block >= e->mach->block_len)
    return true;
  const ny_mach_block_t *b = &e->mach->blocks[block];
  if (!b->inst_count)
    return true;
  size_t last = b->first_inst + b->inst_count - 1;
  e->cur_inst = last;
  for (uint32_t v = 0; v < e->fp_colors_len; ++v) {
    if (!ny_mach_regalloc_live_out(&e->fp_regalloc, block, v) ||
        !e->fp_color_seeded || !e->fp_color_seeded[v])
      continue;
    const ny_mach_live_segment_t *seg = mach_fsegment_at(e, v, last);
    if (!seg || seg->color < 0)
      continue;
    if (!mach_store_xmm(e, mach_vreg_home_off(e, v),
                        e->mach->vreg_types[v] == NY_MACH_TYPE_F32,
                        (unsigned)seg->color))
      return false;
    e->fp_color_seeded[v] = false;
  }
  return true;
}

static bool mach_save_vector_live_out(ny_x64_mach_enc_t *e, size_t block) {
  if (!e || !e->vec_fast_path || block >= e->mach->block_len)
    return true;
  const ny_mach_block_t *b = &e->mach->blocks[block];
  if (!b->inst_count)
    return true;
  size_t last = b->first_inst + b->inst_count - 1;
  e->cur_inst = last;
  for (uint32_t v = 0; v < e->vec_colors_len; ++v) {
    if (!ny_mach_regalloc_live_out(&e->vec_regalloc, block, v) ||
        !e->vec_color_seeded || !e->vec_color_seeded[v])
      continue;
    const ny_mach_live_segment_t *seg = mach_vsegment_at(e, v, last);
    if (!seg || seg->color < 0)
      continue;
    bool is_v256 = e->mach && v < e->mach->vreg_len &&
                   e->mach->vreg_types[v] == NY_MACH_TYPE_V256_I64;
    if (!(is_v256 ? mach_ymm_store_v256(e, mach_vreg_home_off(e, v),
                                        (unsigned)seg->color)
                  : mach_store_xmm_v128(e, mach_vreg_home_off(e, v),
                                        (unsigned)seg->color)))
      return false;
    e->vec_color_seeded[v] = false;
  }
  return true;
}

static bool mach_flush_float_to_home(ny_x64_mach_enc_t *e) {
  if (!e || !e->fp_fast_path || !e->fp_color_seeded)
    return true;
  for (uint32_t v = 0; v < e->fp_colors_len; ++v) {
    if (!e->fp_color_seeded[v])
      continue;
    const ny_mach_live_segment_t *seg =
        mach_fsegment_at(e, v, e->cur_inst);
    if (!seg || seg->color < 0)
      continue;
    if (!mach_store_xmm(e, mach_vreg_home_off(e, v),
                        e->mach->vreg_types[v] == NY_MACH_TYPE_F32,
                        (unsigned)seg->color))
      return false;
    e->fp_color_seeded[v] = false;
  }
  return true;
}

static bool mach_flush_vector_to_home(ny_x64_mach_enc_t *e) {
  if (!e || !e->vec_fast_path || !e->vec_color_seeded)
    return true;
  for (uint32_t v = 0; v < e->vec_colors_len; ++v) {
    if (!e->vec_color_seeded[v])
      continue;
    const ny_mach_live_segment_t *seg = mach_vsegment_at(e, v, e->cur_inst);
    if (!seg || seg->color < 0)
      continue;
    bool is_v256 = e->mach && v < e->mach->vreg_len &&
                   e->mach->vreg_types[v] == NY_MACH_TYPE_V256_I64;
    if (!(is_v256 ? mach_ymm_store_v256(e, mach_vreg_home_off(e, v),
                                        (unsigned)seg->color)
                  : mach_store_xmm_v128(e, mach_vreg_home_off(e, v),
                                        (unsigned)seg->color)))
      return false;
    e->vec_color_seeded[v] = false;
  }
  return true;
}

static bool mach_seed_float_preg(ny_x64_mach_enc_t *e, uint32_t vreg) {
  int preg = mach_vreg_fpreg(e, vreg);
  if (preg < 0 || (e->fp_color_seeded && e->fp_color_seeded[vreg]))
    return true;
  if (!mach_load_xmm(e, mach_vreg_home_off(e, vreg),
                     e->mach->vreg_types[vreg] == NY_MACH_TYPE_F32,
                     (unsigned)preg))
    return false;
  if (e->fp_color_seeded)
    e->fp_color_seeded[vreg] = true;
  return true;
}

static bool mach_load_float_operand(ny_x64_mach_enc_t *e,
                                    const ny_mach_operand_t *op, bool f32,
                                    unsigned scratch, unsigned *out_reg) {
  if (op && op->kind == NY_MACH_OPERAND_VREG) {
    int preg = mach_vreg_fpreg(e, op->as.reg);
    if (preg >= 0) {
      if (!mach_seed_float_preg(e, op->as.reg))
        return false;
      *out_reg = (unsigned)preg;
      return true;
    }
  }
  if (!op || (op->kind != NY_MACH_OPERAND_VREG &&
              op->kind != NY_MACH_OPERAND_FRAME))
    return mach_err(e, "x64 machine form encode: unsupported float operand");
  *out_reg = scratch;
  return mach_load_xmm(e, mach_slot_off(e, op), f32, scratch);
}

static bool mach_float_copy(ny_x64_mach_enc_t *e,
                            const ny_mach_operand_t *dst,
                            const ny_mach_operand_t *src, int dst_off,
                            bool f32) {
  unsigned d = 14, s = 14;
  int assigned = dst && dst->kind == NY_MACH_OPERAND_VREG
                     ? mach_vreg_fpreg(e, dst->as.reg)
                     : -1;
  if (assigned >= 0)
    d = (unsigned)assigned;
  if (src && src->kind == NY_MACH_OPERAND_IMM) {
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

static bool mach_float_binop(ny_x64_mach_enc_t *e,
                             const ny_mach_inst_t *in, int dst_off,
                             bool f32, unsigned opc) {
  unsigned d = 14, a = 14;
  int assigned = in->dst.kind == NY_MACH_OPERAND_VREG
                     ? mach_vreg_fpreg(e, in->dst.as.reg)
                     : -1;
  if (assigned >= 0)
    d = (unsigned)assigned;
  if (!mach_load_float_operand(e, &in->src0, f32, d, &a))
    return false;
  if (a != d && !mach_sse_mov_reg(e, d, a, f32))
    return false;
  if (in->src1.kind == NY_MACH_OPERAND_VREG) {
    int breg = mach_vreg_fpreg(e, in->src1.as.reg);
    if (breg >= 0) {
      if (!mach_seed_float_preg(e, in->src1.as.reg) ||
          !mach_sse_op_reg(e, d, (unsigned)breg, f32, opc))
        return false;
    } else if (!mach_sse_op_mem_reg(e, d, mach_slot_off(e, &in->src1), f32,
                                   opc))
      return false;
  } else if (in->src1.kind == NY_MACH_OPERAND_FRAME) {
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

static bool mach_float_fast_eligible(const ny_mach_func_t *mach) {
  if (!mach)
    return false;
  bool has_float = false;
  for (size_t i = 0; i < mach->inst_len; ++i) {
    const ny_mach_inst_t *in = &mach->insts[i];
    const ny_mach_operand_t *ops[] = {&in->dst, &in->src0, &in->src1,
                                      &in->src2};
    bool inst_float = false;
    for (size_t k = 0; k < sizeof(ops) / sizeof(ops[0]); ++k) {
      const ny_mach_operand_t *op = ops[k];
      if (!op || op->kind != NY_MACH_OPERAND_VREG ||
          op->as.reg >= mach->vreg_len)
        continue;
      ny_mach_type_t t = mach->vreg_types[op->as.reg];
      if (t == NY_MACH_TYPE_F32 || t == NY_MACH_TYPE_F64)
        inst_float = true;
    }
    if (!inst_float)
      continue;
    has_float = true;
    if (in->opcode != NY_MACH_COPY && in->opcode != NY_MACH_ADD &&
        in->opcode != NY_MACH_SUB && in->opcode != NY_MACH_MUL &&
        in->opcode != NY_MACH_DIV && in->opcode != NY_MACH_FMA &&
        in->opcode != NY_MACH_SQRT && in->opcode != NY_MACH_SIN &&
        in->opcode != NY_MACH_COS &&
        in->opcode != NY_MACH_LOAD && in->opcode != NY_MACH_STORE &&
        in->opcode != NY_MACH_CMP &&
        in->opcode != NY_MACH_CALL && in->opcode != NY_MACH_RET)
      return false;
  }
  return has_float;
}

static bool mach_load_eax(ny_x64_mach_enc_t *e, int off) {
  mach_rax_invalidate(e);
  return mach_u8(e, 0x8b) && mach_u8(e, 0x85) && mach_i32(e, off);
}

static bool mach_store_eax_rsp(ny_x64_mach_enc_t *e, int off) {
  return mach_u8(e, 0x89) && mach_u8(e, 0x84) && mach_u8(e, 0x24) &&
         mach_i32(e, off);
}

static bool mach_param_slots(const ny_mach_func_t *mach, bool *params) {
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
    const ny_mach_inst_t *in = &mach->insts[i];
    if (in->opcode == NY_MACH_STORE && in->dst.kind == NY_MACH_OPERAND_FRAME) {
      if (in->dst.as.frame_index >= mach->frame_slot_len) {
        free(stored);
        return false;
      }
      stored[in->dst.as.frame_index] = true;
    } else if (in->opcode == NY_MACH_LOAD &&
               in->src0.kind == NY_MACH_OPERAND_FRAME) {
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

static bool mach_store_gpr(ny_x64_mach_enc_t *e, int off, int reg) {
  unsigned char modrm = (unsigned char)(0x85 | ((reg & 7) << 3));
  return mach_rex(e, true, reg >= 8, false, false) && mach_u8(e, 0x89) &&
         mach_u8(e, modrm) && mach_i32(e, off);
}

static bool mach_spill_params(ny_x64_mach_enc_t *e) {
  const ny_mach_func_t *mach = e->mach;
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
    bool is_f32 = mach->frame_slots[slot].type == NY_MACH_TYPE_F32;
    bool is_f64 = mach->frame_slots[slot].type == NY_MACH_TYPE_F64;
    bool is_fp = is_f32 || is_f64;
    bool from_stack = is_win
                          ? ordinal >= e->target->gp_arg_reg_count
                          : is_fp ? fp_arg >= e->target->fp_arg_reg_count
                                  : gp_arg >= e->target->gp_arg_reg_count;
    int dst = -8 * (e->cs_slots + (int)slot + 1);
    if (from_stack) {
      int incoming = is_win ? 16 + (int)ordinal * 8
                            : 16 + (int)stack_arg * 8;
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
 * SSE scalar op xmm0, mem: F2/F3 0F opc 85 disp — opc: 58 add 5c sub 59 mul 5e div
 */
static bool mach_sse_op_mem(ny_x64_mach_enc_t *e, int off, bool f32, unsigned opc) {
  return mach_u8(e, f32 ? 0xf3 : 0xf2) && mach_u8(e, 0x0f) && mach_u8(e, opc) &&
         mach_u8(e, 0x85) && mach_i32(e, off);
}


static bool ny_mach_profile_hot_loop_block(const ny_mach_func_t *mach,
                                           uint32_t block) {
  if (!mach || block >= mach->block_len)
    return false;
  uint32_t source_pc = mach->blocks[block].source_pc;
  return source_pc != UINT32_MAX && ny_native_profile_loop_hot(source_pc) >= 16;
}

static size_t ny_mach_regalloc_hot_peak_live(const ny_mach_func_t *mach,
                                             const ny_mach_regalloc_t *a,
                                             size_t inst_len) {
  if (!mach || !a || !inst_len)
    return 0;
  int *delta = calloc(inst_len + 1, sizeof(*delta));
  if (!delta)
    return 0;
  for (size_t i = 0; i < a->segment_len; ++i) {
    const ny_mach_live_segment_t *seg = &a->segments[i];
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

static void ny_mach_regalloc_summarize(const ny_mach_func_t *mach,
                                       const ny_mach_regalloc_t *a,
                                       size_t inst_len,
                                       ny_native_regalloc_metrics_t *out) {
  if (!out)
    return;
  *out = (ny_native_regalloc_metrics_t){0};
  if (!a)
    return;
  out->segments = a->segment_len;
  for (size_t i = 0; i < a->segment_len; ++i) {
    const ny_mach_live_segment_t *seg = &a->segments[i];
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
  out->peak_live = ny_mach_regalloc_peak_live(a, inst_len);
  out->hot_loop_peak_live = ny_mach_regalloc_hot_peak_live(mach, a, inst_len);
}

bool ny_native_x64_regalloc_metrics(const ny_mach_func_t *mach,
                                    ny_native_regalloc_metrics_t *gpr,
                                    ny_native_regalloc_metrics_t *fpr,
                                    ny_native_regalloc_metrics_t *vector) {
  if (gpr)
    *gpr = (ny_native_regalloc_metrics_t){0};
  if (fpr)
    *fpr = (ny_native_regalloc_metrics_t){0};
  if (vector)
    *vector = (ny_native_regalloc_metrics_t){0};
  if (!mach)
    return false;

  size_t gp_color_count = MIR_COLOR_N;
  for (size_t i = 0; i < mach->inst_len; ++i) {
    if (mach->insts[i].opcode == NY_MACH_CALL) {
      gp_color_count = MIR_CALLEE_COLOR_N;
      break;
    }
  }

  ny_mach_regalloc_t a = {0};
  if (mach->vreg_len && !ny_mach_regalloc_build(mach, gp_color_count, &a))
    return false;
  ny_mach_regalloc_summarize(mach, &a, mach->inst_len, gpr);
  ny_mach_regalloc_free(&a);

  bool fp_eligible = mach_float_fast_eligible(mach);
  for (size_t i = 0; fp_eligible && i < mach->inst_len; ++i)
    if (mach->insts[i].opcode == NY_MACH_SIN ||
        mach->insts[i].opcode == NY_MACH_COS)
      fp_eligible = false;
  if (mach->vreg_len && fp_eligible) {
    if (!ny_mach_regalloc_build_class(mach, NY_MACH_REGCLASS_FPR,
                                      MFP_COLOR_N, &a)) {
      ny_mach_regalloc_free(&a);
      return false;
    }
    ny_mach_regalloc_summarize(mach, &a, mach->inst_len, fpr);
    ny_mach_regalloc_free(&a);
  }

  if (mach->vreg_len && mach_vector_fast_eligible(mach)) {
    if (!ny_mach_regalloc_build_class(mach, NY_MACH_REGCLASS_VECTOR,
                                      MVEC_COLOR_N, &a)) {
      ny_mach_regalloc_free(&a);
      return false;
    }
    ny_mach_regalloc_summarize(mach, &a, mach->inst_len, vector);
    ny_mach_regalloc_free(&a);
  }
  return true;
}

static bool mach_encode_function(ny_x64_mach_enc_t *e, const char *name,
                                bool tag_return) {
  (void)tag_return;
  const ny_mach_func_t *mach = e->mach;
  e->fp_fast_path = mach_float_fast_eligible(mach);
  for (size_t i = 0; e->fp_fast_path && i < mach->inst_len; ++i)
    if (mach->insts[i].opcode == NY_MACH_SIN ||
        mach->insts[i].opcode == NY_MACH_COS)
      e->fp_fast_path = false;
  e->vec_fast_path = mach_vector_fast_eligible(mach);
  e->last_rax_valid = false;
  e->last_rax_off = 0;
  unsigned save_mask = 0;
  e->regalloc = (ny_mach_regalloc_t){0};
  e->fp_regalloc = (ny_mach_regalloc_t){0};
  e->vec_regalloc = (ny_mach_regalloc_t){0};
  e->color_seeded = NULL;
  e->fp_color_seeded = NULL;
  e->vec_color_seeded = NULL;
  e->cur_inst = 0;
  e->force_stack_reads = false;
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
  /*
   * Vector homes are canonical at CFG joins.  Keep scalar values in their
   * homes while the vector path is active; the scalar color carry path cannot
   * model the widened reduction's edge copies yet.
   */
  e->force_stack_reads = e->vec_fast_path;
  int gp_color_count = MIR_CALLEE_COLOR_N;
  for (size_t i = 0; i < mach->inst_len; ++i) {
    if (mach->insts[i].opcode == NY_MACH_CALL) {
      gp_color_count = MIR_CALLEE_COLOR_N;
      break;
    }
    gp_color_count = MIR_COLOR_N;
  }
  e->colors_len = mach->vreg_len;
  if (mach->vreg_len) {
    if (mach->vreg_len <= MACH_SEEDED_STACK_MAX) {
      e->color_seeded = color_seeded_stack;
      memset(e->color_seeded, 0, mach->vreg_len * sizeof(bool));
    } else {
      e->color_seeded = calloc(mach->vreg_len, sizeof(*e->color_seeded));
    }
    if (!e->color_seeded ||
        !ny_mach_regalloc_build(mach, gp_color_count, &e->regalloc)) {
      if (e->color_seeded && e->color_seeded != color_seeded_stack)
        free(e->color_seeded);
      e->color_seeded = NULL;
      ny_mach_regalloc_free(&e->regalloc);
      return mach_err(e, "x64 machine form encode: register allocation failed");
    }
    for (size_t i = 0; i < e->regalloc.segment_len; ++i) {
      int c = e->regalloc.segments[i].color;
      if (c >= 0 && c < MIR_CALLEE_COLOR_N)
        save_mask |= 1u << c;
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
        !ny_mach_regalloc_build_class(mach, NY_MACH_REGCLASS_FPR,
                                       MFP_COLOR_N, &e->fp_regalloc)) {
      if (e->fp_color_seeded && e->fp_color_seeded != fp_color_seeded_stack)
        free(e->fp_color_seeded);
      e->fp_color_seeded = NULL;
      ny_mach_regalloc_free(&e->fp_regalloc);
      e->fp_fast_path = false;
    }
  }
  e->vec_colors_len = mach->vreg_len;
  if (e->vec_fast_path && mach->vreg_len) {
    if (mach->vreg_len <= MACH_SEEDED_STACK_MAX) {
      e->vec_color_seeded = vec_color_seeded_stack;
      memset(e->vec_color_seeded, 0, mach->vreg_len * sizeof(bool));
    } else {
      e->vec_color_seeded = calloc(mach->vreg_len, sizeof(*e->vec_color_seeded));
    }
    if (!e->vec_color_seeded ||
        !ny_mach_regalloc_build_class(mach, NY_MACH_REGCLASS_VECTOR,
                                       MVEC_COLOR_N, &e->vec_regalloc)) {
      if (e->vec_color_seeded && e->vec_color_seeded != vec_color_seeded_stack)
        free(e->vec_color_seeded);
      e->vec_color_seeded = NULL;
      ny_mach_regalloc_free(&e->vec_regalloc);
      e->vec_fast_path = false;
    }
  }

  /*
   * One-time constant map: NYIR CONST_I64 lowers to COPY vreg <- IMM, which
   * materializes the constant into a stack home.  The ALU/CMP/shift paths
   * below consult this map to fold the value into an immediate operand and
   * skip the per-use home read.  Only vregs defined by an IMM COPY are
   * recorded; everything else falls back to the memory-operand forms.
   */
  int64_t *imm_fold = NULL;
  bool *imm_fold_known = NULL;
  ny_mach_operand_t *addr_fold = NULL;
  bool *addr_fold_known = NULL;
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
      return mach_err(e, "x64 machine form encode: OOM (rematerialization map)");
    }
    for (size_t i = 0; i < mach->inst_len; ++i) {
      const ny_mach_inst_t *ci = &mach->insts[i];
      if (ci->opcode == NY_MACH_COPY &&
          ci->dst.kind == NY_MACH_OPERAND_VREG &&
          ci->src0.kind == NY_MACH_OPERAND_IMM &&
          ci->dst.as.reg < mach->vreg_len) {
        imm_fold[ci->dst.as.reg] = ci->src0.as.imm;
        imm_fold_known[ci->dst.as.reg] = true;
      }
      if (ci->opcode == NY_MACH_LEA &&
          ci->dst.kind == NY_MACH_OPERAND_VREG &&
          ci->dst.as.reg < mach->vreg_len &&
          (ci->src0.kind == NY_MACH_OPERAND_FRAME ||
           ci->src0.kind == NY_MACH_OPERAND_SYMBOL)) {
        addr_fold[ci->dst.as.reg] = ci->src0;
        addr_fold_known[ci->dst.as.reg] = true;
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
   */
  size_t frame_slots_bytes = 0, frame_bytes = 0;
  if (!ny_size_mul_ok(mach->frame_slot_len, 8, &frame_slots_bytes) ||
      !mach_build_vreg_homes(e, frame_slots_bytes) ||
      frame_slots_bytes > SIZE_MAX - e->vreg_home_bytes)
    return mach_err(e, "x64 machine form encode: frame size overflow");
  frame_bytes = frame_slots_bytes + e->vreg_home_bytes;
  if (frame_bytes > (size_t)INT_MAX - 23u)
    return mach_err(e, "x64 machine form encode: frame too large");
  int frame = (int)((frame_bytes + 15u) & ~(size_t)15u);
  if ((e->cs_slots & 1) != 0)
    frame += 8;
  if (((size_t)e->cs_slots * 8u + (size_t)frame) % 16u != 0)
    return mach_err(e, "x64 machine form encode: internal stack alignment error");

  /*
   * push %rbp; mov %rsp,%rbp
   */
  if (!mach_u8(e, 0x55) || !mach_rex(e, true, false, false, false) || !mach_u8(e, 0x89) ||
      !mach_u8(e, 0xe5))
    return false;
  if (!mach_push_callee_saves(e, save_mask))
    return false;
  if (frame) {
    /*
     * sub $frame, %rsp
     */
    if (!mach_rex(e, true, false, false, false) || !mach_u8(e, 0x81) || !mach_u8(e, 0xec) ||
        !mach_i32(e, frame))
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
  if (base_name && strcmp(base_name, "main") == 0 &&
      e->target && e->target->abi == NY_NATIVE_ABI_SYSV) {
    char set_args[256];
    snprintf(set_args, sizeof(set_args), "%s_ny_aot_set_args",
             symbol_prefix);
    if (!mach_u8(e, 0xe8) ||
        !mach_add_reloc(e, set_args, NY_RELOC_PLT32) ||
        !mach_i32(e, 0))
      return false;
  }
  if (base_name && strcmp(base_name, "rt_main") != 0 &&
      strcmp(base_name, "main") != 0 && !mach_spill_params(e))
    return false;

  e->block_off = calloc(mach->block_len ? mach->block_len : 1, sizeof(size_t));
  if (!e->block_off && mach->block_len)
    return mach_err(e, "x64 machine form encode: OOM");
  for (size_t bi = 0; bi < mach->block_len; ++bi) {
    e->block_off[bi] = e->code.len;
    /*
     * Invalidate the sticky-rax cache on every control-flow edge: rax may
     * hold any predecessor's last-loaded slot here, and claiming it holds
     * this block's first slot would elide a needed load.
     */
    mach_rax_invalidate(e);
    e->fused_jcc = 0; /* no flags-only CMP pending across a block edge */
    const ny_mach_block_t *blk = &mach->blocks[bi];
    /*
     * Ordinary register residency is a runtime-predecessor property. Carried
     * segments are the exception: loop-edge coalescing proved that every
     * incoming edge supplies the same physical register.
     */
    if (e->color_seeded && e->colors_len)
      memset(e->color_seeded, 0, e->colors_len * sizeof(*e->color_seeded));
    for (uint32_t v = 0; v < e->colors_len; ++v) {
      const ny_mach_live_segment_t *seg =
          mach_segment_at(e, v, blk->first_inst);
      if (seg && seg->carried && seg->color >= 0)
        e->color_seeded[v] = true;
    }
    for (size_t n = 0; n < blk->inst_count; ++n) {
      e->cur_inst = blk->first_inst + n;
      e->pending_rax_valid = false;
      const ny_mach_inst_t *in = &mach->insts[e->cur_inst];
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
       * Control-flow terminator: successors (phi-edge copy blocks, loop
       * headers) reload scalar live-out values from their canonical homes,
       * so those home commits must be emitted BEFORE the branch bytes.
       * The post-block mach_save_live_out below would place them after the
       * terminator, in dead fall-through space, leaving entry/backedge
       * homes uninitialized (O2 fib returned garbage / spun).  Float and
       * vector live-outs are already flushed inside the BR/BR_IF cases.
       */
      if ((in->opcode == NY_MACH_BR || in->opcode == NY_MACH_BR_IF) &&
          !mach_save_live_out(e, bi))
        return false;
      switch (in->opcode) {
      case NY_MACH_COPY:
        if (mach_is_v128(mach, &in->dst)) {
          ny_mach_type_t vt = mach_v128_type(mach, &in->dst);
          if (e->vec_fast_path && vt != NY_MACH_TYPE_V256_I64) {
            if (!mach_vector_copy(e, &in->dst, &in->src0, dst))
              return false;
            break;
          }
          if (e->vec_fast_path && vt == NY_MACH_TYPE_V256_I64 &&
              in->src0.kind != NY_MACH_OPERAND_IMM) {
            if (!mach_vector_ymm_copy(e, &in->dst, &in->src0, dst))
              return false;
            break;
          }
          if (in->src0.kind == NY_MACH_OPERAND_IMM) {
            /*
             * Broadcast imm to both qwords via two stores.
             */
            if (!mach_mov_rax_imm(e, in->src0.as.imm) || !mach_store_rax(e, dst) ||
                !mach_store_rax(e, dst - 8))
              return false;
          } else if (mach_is_v128(mach, &in->src0)) {
            if (!mach_load_xmm0_v128(e, a) || !mach_store_xmm0_v128(e, dst))
              return false;
          } else if (vt == NY_MACH_TYPE_V128_I64) {
            /*
             * SET1_I64: movq scalar→xmm0; punpcklqdq self (broadcast).
             */
            if (!mach_load_rax(e, a) || !mach_u8(e, 0x66) || !mach_rex(e, true, false, false, false) ||
                !mach_u8(e, 0x0f) || !mach_u8(e, 0x6e) || !mach_u8(e, 0xc0) ||
                !mach_u8(e, 0x66) || !mach_u8(e, 0x0f) || !mach_u8(e, 0x6c) ||
                !mach_u8(e, 0xc0) || !mach_store_xmm0_v128(e, dst))
              return false;
          } else if (vt == NY_MACH_TYPE_V256_I64) {
            /*
             * SET1_I64 ymm: vpbroadcastq ymm0, [rbp+a]
             * VEX.256.66.0F38.W1 59 /r → C4 E3 FD 59 85 disp32
             */
            if (!mach_u8(e, 0xc4) || !mach_u8(e, 0xe2) ||
                !mach_u8(e, 0xfd) || !mach_u8(e, 0x59) ||
                !mach_u8(e, 0x85) || !mach_i32(e, a) ||
                !mach_u8(e, 0xc4) || !mach_u8(e, 0xe1) ||
                !mach_u8(e, 0x7d) || !mach_u8(e, 0x7f) ||
                !mach_u8(e, 0x85) || !mach_i32(e, dst))
              return false;
          } else if (vt == NY_MACH_TYPE_V128_F64) {
            /*
             * SET1_F64: movddup.
             */
            if (!mach_load_xmm0(e, a, false) || !mach_u8(e, 0xf2) ||
                !mach_u8(e, 0x0f) || !mach_u8(e, 0x12) || !mach_u8(e, 0xc0) ||
                !mach_store_xmm0_v128(e, dst))
              return false;
          } else if (vt == NY_MACH_TYPE_V128_F32) {
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
          bool bitwise_gpr_source =
              in->src0.kind != NY_MACH_OPERAND_IMM &&
              !mach_is_float(mach, &in->src0);
          if (e->fp_fast_path && !bitwise_gpr_source) {
            if (!mach_float_copy(e, &in->dst, &in->src0, dst, f32))
              return false;
          } else if (in->src0.kind == NY_MACH_OPERAND_IMM) {
            if (!mach_mov_rax_imm(e, in->src0.as.imm) || !mach_u8(e, 0x66) ||
                !mach_rex(e, true, false, false, false) || !mach_u8(e, 0x0f) || !mach_u8(e, 0x6e) ||
                !mach_u8(e, 0xc0) || !mach_store_xmm0(e, dst, f32))
              return false;
          } else if (!mach_load_xmm0(e, a, f32) || !mach_store_xmm0(e, dst, f32))
            return false;
          break;
        }
        if (!mach_is_i64(mach, &in->dst) && in->dst.kind != NY_MACH_OPERAND_NONE)
          return mach_err(e, "x64 machine form encode: unsupported COPY type");
        /*
         * Loop-carried phi copy: the source already resides in the physical
         * register assigned to the destination, so the copy is a no-op.
         */
        if (in->src0.kind == NY_MACH_OPERAND_VREG &&
            in->dst.kind == NY_MACH_OPERAND_VREG && e->color_seeded &&
            in->src0.as.reg < e->colors_len &&
            in->dst.as.reg < e->colors_len) {
          int sp = mach_vreg_preg(e, in->src0.as.reg);
          int dp = mach_vreg_preg(e, in->dst.as.reg);
          /*
           * Only elide a same-preg copy when the carry pass explicitly
           * validated the chain (dst segment marked carried).  Without the
           * carried flag the elision would skip the home commit that a
           * later block's live-in reload depends on, leaving the canonical
           * slot stale and the loop reading garbage.
           */
          const ny_mach_live_segment_t *dseg =
              mach_segment_at(e, in->dst.as.reg, e->cur_inst);
          if (sp >= 0 && sp == dp && dseg &&
              e->color_seeded[in->src0.as.reg] &&
              (dseg->carried ||
               !mach_vreg_future_use_requires_home(e, in->dst.as.reg))) {
            /*
             * Post-allocation copy coalescing: the destination already lives
             * in the required register.  Keep the explicit copy only when a
             * same-segment consumer requires the destination's stack home.
             */
            e->color_seeded[in->dst.as.reg] = true;
            break;
          }
        }
        if (!mach_load_op_rax(e, &in->src0))
          return false;
        if (!mach_commit_vreg(e, &in->dst, dst))
          return false;
        break;
      case NY_MACH_LOAD:
        if (mach_is_v128(mach, &in->dst)) {
          ny_mach_type_t vt = mach_v128_type(mach, &in->dst);
          if (e->vec_fast_path && vt != NY_MACH_TYPE_V256_I64) {
            if (!mach_vector_load(e, in, dst))
              return false;
            break;
          }
          if (e->vec_fast_path && vt == NY_MACH_TYPE_V256_I64) {
            if (!mach_vector_ymm_load(e, in, dst))
              return false;
            break;
          }
          if (vt == NY_MACH_TYPE_V256_I64) {
            /*
             * vmovdqa ymm0, [rbp+disp32] — VEX.256.66.0F 6F /r
             */
            if (in->src0.kind == NY_MACH_OPERAND_FRAME) {
              if (!mach_u8(e, 0xc4) || !mach_u8(e, 0xe1) ||
                  !mach_u8(e, 0x7e) || !mach_u8(e, 0x6f) ||
                  !mach_u8(e, 0x85) || !mach_i32(e, a))
                return false;
            } else if (in->src0.kind == NY_MACH_OPERAND_VREG) {
              if (!mach_load_rax(e, a) || !mach_u8(e, 0xc4) ||
                  !mach_u8(e, 0xe1) || !mach_u8(e, 0x7e) ||
                  !mach_u8(e, 0x6f) || !mach_u8(e, 0x00))
                return false;
            } else
              return mach_err(e, "x64 machine form encode: v256 load");
            /*
             * Store ymm0 back to home.
             */
            if (!mach_u8(e, 0xc4) || !mach_u8(e, 0xe1) ||
                !mach_u8(e, 0x7e) || !mach_u8(e, 0x7f) ||
                !mach_u8(e, 0x85) || !mach_i32(e, dst))
              return false;
            break;
          }
          if (in->src0.kind == NY_MACH_OPERAND_FRAME) {
            if (!mach_load_xmm0_v128(e, a) || !mach_store_xmm0_v128(e, dst))
              return false;
          } else if (in->src0.kind == NY_MACH_OPERAND_VREG) {
            if (!mach_load_rax(e, a) || !mach_u8(e, 0xf3) || !mach_u8(e, 0x0f) ||
                !mach_u8(e, 0x6f) || !mach_u8(e, 0x00) ||
                !mach_store_xmm0_v128(e, dst))
              return false;
          } else
            return mach_err(e, "x64 machine form encode: v128 load");
          break;
        }
        if (mach_is_float(mach, &in->dst)) {
          bool f32 = mach_is_f32(mach, &in->dst);
          if (e->fp_fast_path && in->src0.kind == NY_MACH_OPERAND_FRAME) {
            int dreg = in->dst.kind == NY_MACH_OPERAND_VREG
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
          } else if (in->src0.kind == NY_MACH_OPERAND_FRAME) {
            if (!mach_load_xmm0(e, a, f32) || !mach_store_xmm0(e, dst, f32))
              return false;
          } else if (in->src0.kind == NY_MACH_OPERAND_VREG) {
            /*
             * mov ptr to rax; movs[sd] (rax), xmm0; store
             */
            int dreg = e->fp_fast_path && in->dst.kind == NY_MACH_OPERAND_VREG
                           ? mach_vreg_fpreg(e, in->dst.as.reg)
                           : -1;
            unsigned reg = dreg >= 0 ? (unsigned)dreg : 14;
            if (!mach_load_op_rax(e, &in->src0) || !mach_load_xmm_ptr(e, reg, f32))
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
        if (in->src0.kind == NY_MACH_OPERAND_FRAME) {
          if (!mach_load_rax(e, a) || !mach_commit_vreg(e, &in->dst, dst) ||
              (in->dst.kind == NY_MACH_OPERAND_VREG && !mach_store_rax(e, dst)))
            return false;
        } else {
          if (!mach_load_op_rax(e, &in->src0))
            return false;
          if (in->byte_width) {
            if (!mach_u8(e, 0x0f) || !mach_u8(e, 0xb6) || !mach_u8(e, 0x00))
              return false;
          } else if (!mach_rex(e, true, false, false, false) ||
                     !mach_u8(e, 0x8b) || !mach_u8(e, 0x00)) {
            return false;
          }
          if (!mach_commit_vreg(e, &in->dst, dst) ||
              (in->dst.kind == NY_MACH_OPERAND_VREG && !mach_store_rax(e, dst)))
            return false;
        }
        break;
      case NY_MACH_STORE:
        if (mach_is_v128(mach, &in->src0)) {
          ny_mach_type_t vt = mach_v128_type(mach, &in->src0);
          if (e->vec_fast_path && vt != NY_MACH_TYPE_V256_I64) {
            if (!mach_vector_store(e, in, dst))
              return false;
            break;
          }
          if (e->vec_fast_path && vt == NY_MACH_TYPE_V256_I64) {
            if (!mach_vector_ymm_store(e, in, dst))
              return false;
            break;
          }
          if (vt == NY_MACH_TYPE_V256_I64) {
            /*
             * Load src0 to ymm0, then vmovdqa [dest], ymm0.
             */
            if (in->src0.kind == NY_MACH_OPERAND_FRAME) {
              if (!mach_u8(e, 0xc4) || !mach_u8(e, 0xe1) ||
                  !mach_u8(e, 0x7e) || !mach_u8(e, 0x6f) ||
                  !mach_u8(e, 0x85) || !mach_i32(e, a))
                return false;
            } else if (in->src0.kind == NY_MACH_OPERAND_VREG) {
              if (!mach_load_rax(e, a) || !mach_u8(e, 0xc4) ||
                  !mach_u8(e, 0xe1) || !mach_u8(e, 0x7e) ||
                  !mach_u8(e, 0x6f) || !mach_u8(e, 0x00))
                return false;
            } else
              return mach_err(e, "x64 machine form encode: v256 store src");
            if (in->dst.kind == NY_MACH_OPERAND_FRAME) {
              if (!mach_u8(e, 0xc4) || !mach_u8(e, 0xe1) ||
                  !mach_u8(e, 0x7e) || !mach_u8(e, 0x7f) ||
                  !mach_u8(e, 0x85) || !mach_i32(e, dst))
                return false;
            } else if (in->dst.kind == NY_MACH_OPERAND_VREG) {
              if (!mach_load_rax(e, dst) || !mach_u8(e, 0xc4) ||
                  !mach_u8(e, 0xe1) || !mach_u8(e, 0x7e) ||
                  !mach_u8(e, 0x7f) || !mach_u8(e, 0x00))
                return false;
            } else
              return mach_err(e, "x64 machine form encode: v256 store");
            break;
          }
          if (in->dst.kind == NY_MACH_OPERAND_FRAME) {
            if (!mach_load_xmm0_v128(e, a) || !mach_store_xmm0_v128(e, dst))
              return false;
          } else if (in->dst.kind == NY_MACH_OPERAND_VREG) {
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
          if (in->dst.kind == NY_MACH_OPERAND_FRAME) {
            if (e->fp_fast_path) {
              unsigned reg = 14;
              if (!mach_load_float_operand(e, &in->src0, f32, 14, &reg) ||
                  !mach_store_xmm(e, dst, f32, reg))
                return false;
            } else if (!mach_load_xmm0(e, a, f32) || !mach_store_xmm0(e, dst, f32))
              return false;
          } else if (in->dst.kind == NY_MACH_OPERAND_VREG) {
            unsigned reg = 14;
            if (e->fp_fast_path) {
              if (!mach_load_ptr_rcx(e, &in->dst) ||
                  !mach_load_float_operand(e, &in->src0, f32, 14, &reg) ||
                  !mach_store_xmm_ptr(e, reg, f32))
                return false;
            } else if (!mach_load_ptr_rcx(e, &in->dst) ||
                       !mach_load_xmm0(e, a, f32) ||
                       !mach_store_xmm_ptr(e, 0, f32))
              return false;
          }
          break;
        }
        if (!mach_load_op_rax(e, &in->src0))
          return false;
        if (in->dst.kind == NY_MACH_OPERAND_FRAME) {
          if (!mach_store_rax(e, dst))
            return false;
        } else {
          if (!mach_mov_reg_rax(e, 10) ||
              !mach_load_op_rax(e, &in->dst))
            return false;
          if (in->byte_width) {
            if (!mach_rex(e, false, true, false, false) ||
                !mach_u8(e, 0x88) || !mach_u8(e, 0x10))
              return false;
          } else if (!mach_rex(e, true, true, false, false) ||
                     !mach_u8(e, 0x89) || !mach_u8(e, 0x10)) {
            return false;
          }
        }
        break;
      case NY_MACH_LEA:
        if (in->src0.kind == NY_MACH_OPERAND_FRAME) {
          if (!mach_rex(e, true, false, false, false) || !mach_u8(e, 0x8d) || !mach_u8(e, 0x85) ||
              !mach_i32(e, a) || !mach_commit_vreg(e, &in->dst, dst))
            return false;
        } else if (in->src0.kind == NY_MACH_OPERAND_SYMBOL && in->src0.as.symbol) {
          /*
           * leaq sym(%rip), %rax — 48 8d 05 rel32
           */
          if (!mach_rex(e, true, false, false, false) || !mach_u8(e, 0x8d) || !mach_u8(e, 0x05) ||
              !mach_add_reloc(e, in->src0.as.symbol, NY_RELOC_PC32) ||
              !mach_i32(e, 0) || !mach_commit_vreg(e, &in->dst, dst))
            return false;
        } else
          return mach_err(e, "x64 machine form encode: bad LEA");
        break;
      case NY_MACH_CONVERT:
        if (mach_is_f64(mach, &in->dst) && mach_is_i64(mach, &in->src0)) {
          /*
           * cvtsi2sdq %rax, %xmm0
           */
          if (!mach_load_rax(e, a) || !mach_u8(e, 0xf2) || !mach_rex(e, true, false, false, false) ||
              !mach_u8(e, 0x0f) || !mach_u8(e, 0x2a) || !mach_u8(e, 0xc0) ||
              !mach_store_xmm0(e, dst, false))
            return false;
        } else if (mach_is_f32(mach, &in->dst) && mach_is_i64(mach, &in->src0)) {
          if (!mach_load_rax(e, a) || !mach_u8(e, 0xf3) || !mach_rex(e, true, false, false, false) ||
              !mach_u8(e, 0x0f) || !mach_u8(e, 0x2a) || !mach_u8(e, 0xc0) ||
              !mach_store_xmm0(e, dst, true))
            return false;
        } else if (mach_is_f64(mach, &in->dst) && mach_is_f32(mach, &in->src0)) {
          if (!mach_load_xmm0(e, a, true) || !mach_u8(e, 0xf3) || !mach_u8(e, 0x0f) ||
              !mach_u8(e, 0x5a) || !mach_u8(e, 0xc0) || !mach_store_xmm0(e, dst, false))
            return false;
        } else if (mach_is_f32(mach, &in->dst) && mach_is_f64(mach, &in->src0)) {
          if (!mach_load_xmm0(e, a, false) || !mach_u8(e, 0xf2) || !mach_u8(e, 0x0f) ||
              !mach_u8(e, 0x5a) || !mach_u8(e, 0xc0) || !mach_store_xmm0(e, dst, true))
            return false;
        } else if (mach_is_i64(mach, &in->dst) &&
                   (mach_is_f64(mach, &in->src0) ||
                    mach_is_f32(mach, &in->src0))) {
          bool f32 = mach_is_f32(mach, &in->src0);
          if (!mach_load_xmm0(e, a, f32) ||
              !mach_u8(e, f32 ? 0xf3 : 0xf2) ||
              !mach_rex(e, true, false, false, false) ||
              !mach_u8(e, 0x0f) || !mach_u8(e, 0x2c) ||
              !mach_u8(e, 0xc0) ||
              !mach_commit_vreg(e, &in->dst, dst))
            return false;
        } else
          return mach_err(e, "x64 machine form encode: unsupported CONVERT");
        break;
      case NY_MACH_SQRT:
        if (mach_is_f64(mach, &in->dst) && mach_is_f64(mach, &in->src0)) {
          if (!mach_load_xmm0(e, a, false) ||
              !mach_u8(e, 0xf2) || !mach_u8(e, 0x0f) ||
              !mach_u8(e, 0x51) || !mach_u8(e, 0xc0) ||
              !mach_store_xmm0(e, dst, false))
            return false;
        } else if (mach_is_f32(mach, &in->dst) && mach_is_f32(mach, &in->src0)) {
          if (!mach_load_xmm0(e, a, true) ||
              !mach_u8(e, 0xf3) || !mach_u8(e, 0x0f) ||
              !mach_u8(e, 0x51) || !mach_u8(e, 0xc0) ||
              !mach_store_xmm0(e, dst, true))
            return false;
        } else
          return mach_err(e, "x64 machine form encode: unsupported SQRT type");
        break;
      case NY_MACH_SIN:
      case NY_MACH_COS:
        if (!mach_is_f64(mach, &in->dst) || !mach_is_f64(mach, &in->src0))
          return mach_err(e, "x64 machine form encode: unsupported trig type");
        if (!mach_u8(e, 0xdd) || !mach_u8(e, 0x85) || !mach_i32(e, a) ||
            !mach_u8(e, 0xd9) ||
            !mach_u8(e, in->opcode == NY_MACH_SIN ? 0xfe : 0xff) ||
            !mach_u8(e, 0xdd) || !mach_u8(e, 0x9d) || !mach_i32(e, dst))
          return false;
        break;
      case NY_MACH_ADD:
      case NY_MACH_SUB:
      case NY_MACH_AND:
      case NY_MACH_OR:
      case NY_MACH_XOR:
      case NY_MACH_MUL:
      case NY_MACH_DIV:
      case NY_MACH_SHL:
      case NY_MACH_SAR:
        if (mach_is_v128(mach, &in->dst)) {
          ny_mach_type_t vt = mach_v128_type(mach, &in->dst);
          if (e->vec_fast_path && vt != NY_MACH_TYPE_V256_I64) {
            unsigned prefix = vt == NY_MACH_TYPE_V128_I64 ||
                                      vt == NY_MACH_TYPE_V128_F64
                                  ? 0x66
                                  : 0;
            unsigned opc = in->opcode == NY_MACH_ADD ? 0x58
                          : in->opcode == NY_MACH_SUB ? 0x5c
                          : in->opcode == NY_MACH_MUL ? 0x59
                          : in->opcode == NY_MACH_DIV ? 0x5e
                          : vt == NY_MACH_TYPE_V128_I64 &&
                                    in->opcode == NY_MACH_AND
                                ? 0xdb
                          : vt == NY_MACH_TYPE_V128_I64 &&
                                    in->opcode == NY_MACH_OR
                                ? 0xeb
                          : vt == NY_MACH_TYPE_V128_I64 &&
                                    in->opcode == NY_MACH_XOR
                                ? 0xef
                                : 0xff;
            if (opc == 0xff ||
                (vt != NY_MACH_TYPE_V128_I64 &&
                 (in->opcode == NY_MACH_AND || in->opcode == NY_MACH_OR ||
                  in->opcode == NY_MACH_XOR)))
              return mach_err(e, "x64 machine form encode: unsupported vector ALU");
            if (!mach_vector_binop(e, in, dst, prefix, opc))
              return false;
            break;
          }
          if (e->vec_fast_path && vt == NY_MACH_TYPE_V256_I64 &&
              in->opcode != NY_MACH_MUL && in->opcode != NY_MACH_DIV &&
              in->opcode != NY_MACH_SHL && in->opcode != NY_MACH_SAR) {
            unsigned char opc = in->opcode == NY_MACH_ADD ? 0xd4
                              : in->opcode == NY_MACH_SUB ? 0xfb
                              : in->opcode == NY_MACH_AND ? 0xdb
                              : in->opcode == NY_MACH_OR  ? 0xeb
                                                         : 0xef;
            if (!mach_vector_ymm_binop(e, in, dst, opc))
              return false;
            break;
          }
          if (!mach_load_xmm0_v128(e, a))
            return false;
          if (vt == NY_MACH_TYPE_V128_I64) {
            /*
             * paddq/psubq/pand/por/pxor — no packed i64 mul/div.
             */
            if (in->opcode == NY_MACH_MUL || in->opcode == NY_MACH_DIV)
              return mach_err(e, "x64 machine form encode: no packed i64 mul/div");
            unsigned char opc = in->opcode == NY_MACH_ADD ? 0xd4
                              : in->opcode == NY_MACH_SUB ? 0xfb
                              : in->opcode == NY_MACH_AND ? 0xdb
                              : in->opcode == NY_MACH_OR  ? 0xeb
                                                         : 0xef;
            if (!mach_u8(e, 0x66) || !mach_u8(e, 0x0f) || !mach_u8(e, opc) ||
                !mach_u8(e, 0x85) || !mach_i32(e, b) ||
                !mach_store_xmm0_v128(e, dst))
              return false;
          } else if (vt == NY_MACH_TYPE_V256_I64) {
            /*
             * vpaddq/vpsubq/vpand/vpor/vpxor ymm — VEX.256.66.0F.W1 opc
             */
            if (in->opcode == NY_MACH_MUL || in->opcode == NY_MACH_DIV)
              return mach_err(e, "x64 machine form encode: no packed i64 mul/div");
            unsigned char opc = in->opcode == NY_MACH_ADD ? 0xd4
                              : in->opcode == NY_MACH_SUB ? 0xfb
                              : in->opcode == NY_MACH_AND ? 0xdb
                              : in->opcode == NY_MACH_OR  ? 0xeb
                                                         : 0xef;
            if (!mach_u8(e, 0xc4) || !mach_u8(e, 0xe1) ||
                !mach_u8(e, 0x7d) || !mach_u8(e, opc) ||
                !mach_u8(e, 0x85) || !mach_i32(e, b) ||
                !mach_u8(e, 0xc4) || !mach_u8(e, 0xe1) ||
                !mach_u8(e, 0x7d) || !mach_u8(e, 0x7f) ||
                !mach_u8(e, 0x85) || !mach_i32(e, dst))
              return false;
          } else if (vt == NY_MACH_TYPE_V128_F64) {
            /*
             * addpd/subpd/mulpd/divpd — 66 0F opc
             */
            if (in->opcode == NY_MACH_AND || in->opcode == NY_MACH_OR ||
                in->opcode == NY_MACH_XOR)
              return mach_err(e, "x64 machine form encode: no packed f64 bitwise");
            unsigned char opc = in->opcode == NY_MACH_ADD ? 0x58
                              : in->opcode == NY_MACH_SUB ? 0x5c
                              : in->opcode == NY_MACH_MUL ? 0x59
                              : in->opcode == NY_MACH_DIV ? 0x5e
                                                         : 0xff;
            if (opc == 0xff || !mach_u8(e, 0x66) || !mach_u8(e, 0x0f) ||
                !mach_u8(e, opc) || !mach_u8(e, 0x85) || !mach_i32(e, b) ||
                !mach_store_xmm0_v128(e, dst))
              return false;
          } else if (vt == NY_MACH_TYPE_V128_F32) {
            /*
             * addps/subps/mulps/divps — 0F opc
             */
            if (in->opcode == NY_MACH_AND || in->opcode == NY_MACH_OR ||
                in->opcode == NY_MACH_XOR)
              return mach_err(e, "x64 machine form encode: no packed f32 bitwise");
            unsigned char opc = in->opcode == NY_MACH_ADD ? 0x58
                              : in->opcode == NY_MACH_SUB ? 0x5c
                              : in->opcode == NY_MACH_MUL ? 0x59
                              : in->opcode == NY_MACH_DIV ? 0x5e
                                                         : 0xff;
            if (opc == 0xff || !mach_u8(e, 0x0f) || !mach_u8(e, opc) ||
                !mach_u8(e, 0x85) || !mach_i32(e, b) ||
                !mach_store_xmm0_v128(e, dst))
              return false;
          } else
            return mach_err(e, "x64 machine form encode: bad v128 ALU type");
          break;
        }
        if (in->opcode == NY_MACH_SHL || in->opcode == NY_MACH_SAR) {
          if (mach_is_v128(mach, &in->dst)) {
            if (!e->vec_fast_path)
              return mach_err(e, "x64 machine form encode: vector shift requires fast path");
            ny_mach_type_t vt = mach_v128_type(mach, &in->dst);
            bool is_shift_left = in->opcode == NY_MACH_SHL;
            unsigned prefix = (vt == NY_MACH_TYPE_V128_I64 ||
                               vt == NY_MACH_TYPE_V128_F64) ? 0x66 : 0;
            unsigned opc;
            if (vt == NY_MACH_TYPE_V128_I64 || vt == NY_MACH_TYPE_V128_F64) {
              opc = is_shift_left ? 0xf3 : 0xe3;
            } else if (vt == NY_MACH_TYPE_V128_F32) {
              opc = is_shift_left ? 0xf2 : 0xe2;
            } else {
              return mach_err(e, "x64 machine form encode: unsupported vector shift type");
            }
            if (!mach_vector_shift(e, in, dst, prefix, opc))
              return false;
            break;
          }
          /*
           * Scalar 64-bit shift: dst = a (<op> count) with count in cl or imm.
           */
          if (!mach_is_i64(mach, &in->dst))
            return mach_err(e, "x64 machine form encode: scalar shift needs i64");
          if (!mach_load_op_rax(e, &in->src0))
            return false;
          unsigned op = in->opcode == NY_MACH_SHL ? 0xe0 : 0xf8; /* /4|/7 */
          int64_t fold = 0;
          bool fold_ok = false;
          if (in->src1.kind == NY_MACH_OPERAND_IMM) {
            fold = in->src1.as.imm;
            fold_ok = fold >= 0 && fold <= 63;
          } else if (in->src1.kind == NY_MACH_OPERAND_VREG && imm_fold_known &&
                     in->src1.as.reg < mach->vreg_len &&
                     imm_fold_known[in->src1.as.reg]) {
            /*
             * Fold COPY-from-IMM counts (x86 masks to 6 bits; stay in range).
             */
            fold = imm_fold[in->src1.as.reg];
            fold_ok = fold >= 0 && fold <= 63;
          }
          if (fold_ok) {
            if (!mach_rex(e, true, false, false, false) || !mach_u8(e, 0xc1) || !mach_u8(e, op) ||
                !mach_u8(e, (unsigned char)(fold & 0xff)))
              return false;
          } else {
            if (!mach_load_rcx(e, b) || !mach_rex(e, true, false, false, false) || !mach_u8(e, 0xd3) ||
                !mach_u8(e, op))
              return false;
          }
          if (!mach_commit_vreg(e, &in->dst, dst))
            return false;
          break;
        }
        if (in->opcode == NY_MACH_DIV)
          goto mach_div_scalar;
        if (mach_is_float(mach, &in->dst)) {
          bool f32 = mach_is_f32(mach, &in->dst);
          unsigned opc = in->opcode == NY_MACH_ADD ? 0x58
                        : in->opcode == NY_MACH_SUB ? 0x5c
                        : in->opcode == NY_MACH_MUL ? 0x59 : 0xff;
          if (opc == 0xff || in->opcode == NY_MACH_AND || in->opcode == NY_MACH_OR ||
              in->opcode == NY_MACH_XOR)
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
        if (in->dst.kind == NY_MACH_OPERAND_VREG &&
            in->src0.kind == NY_MACH_OPERAND_VREG) {
          int dp = mach_vreg_preg(e, in->dst.as.reg);
          int ap = mach_vreg_preg(e, in->src0.as.reg);
          if (dp >= 0 && ap >= 0 &&
              !mach_seed_colored_preg(e, in->src0.as.reg))
            return false;
          if (dp >= 0 && ap >= 0) {
            int64_t fold = 0;
            bool fold_ok = false;
            if (in->src1.kind == NY_MACH_OPERAND_IMM) {
              fold = in->src1.as.imm;
              fold_ok = (int64_t)(int32_t)fold == fold;
            } else if (in->src1.kind == NY_MACH_OPERAND_VREG &&
                       imm_fold_known &&
                       in->src1.as.reg < mach->vreg_len &&
                       imm_fold_known[in->src1.as.reg]) {
              fold = imm_fold[in->src1.as.reg];
              fold_ok = (int64_t)(int32_t)fold == fold;
            }
            bool emitted = false;
            if (fold_ok) {
              if (!mach_mov_preg_preg(e, dp, ap) ||
                  !mach_alu_preg_imm(e, in->opcode, dp, fold))
                return false;
              emitted = true;
            } else if (in->src1.kind == NY_MACH_OPERAND_VREG) {
              if (!mach_seed_colored_preg(e, in->src1.as.reg))
                return false;
              int bp = mach_vreg_preg(e, in->src1.as.reg);
              if (bp >= 0) {
                bool commutative = in->opcode != NY_MACH_SUB;
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
            if (emitted) {
              if (mach_vreg_future_use_requires_home(e, in->dst.as.reg)) {
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
        }
        /*
         * Keep a loop-carried multiply/add chain in its resident preg when
         * both intermediates have one immediate src0 consumer.  This is the
         * scalar form of x *= K; x += C before a modulus: it avoids moving
         * the carried state into %rax and avoids two transient stack homes.
         */
        if (in->opcode == NY_MACH_MUL &&
            in->src0.kind == NY_MACH_OPERAND_VREG &&
            in->dst.kind == NY_MACH_OPERAND_VREG &&
            n + 2 < blk->inst_count) {
          int preg = mach_vreg_preg(e, in->src0.as.reg);
          const ny_mach_live_segment_t *src_seg =
              mach_segment_at(e, in->src0.as.reg, e->cur_inst);
          size_t next_index = e->cur_inst + 1;
          const ny_mach_inst_t *next = &mach->insts[next_index];
          const ny_mach_inst_t *consumer = &mach->insts[next_index + 1];
          int64_t mul_imm = 0, add_imm = 0;
          bool mul_const = false, add_const = false, src_reused = false;
          if (in->src1.kind == NY_MACH_OPERAND_IMM) {
            mul_imm = in->src1.as.imm;
            mul_const = (int64_t)(int32_t)mul_imm == mul_imm;
          } else if (in->src1.kind == NY_MACH_OPERAND_VREG &&
                     imm_fold_known &&
                     in->src1.as.reg < mach->vreg_len &&
                     imm_fold_known[in->src1.as.reg]) {
            mul_imm = imm_fold[in->src1.as.reg];
            mul_const = (int64_t)(int32_t)mul_imm == mul_imm;
          }
          if (next->src1.kind == NY_MACH_OPERAND_IMM) {
            add_imm = next->src1.as.imm;
            add_const = (int64_t)(int32_t)add_imm == add_imm;
          } else if (next->src1.kind == NY_MACH_OPERAND_VREG &&
                     imm_fold_known &&
                     next->src1.as.reg < mach->vreg_len &&
                     imm_fold_known[next->src1.as.reg]) {
            add_imm = imm_fold[next->src1.as.reg];
            add_const = (int64_t)(int32_t)add_imm == add_imm;
          }
          for (size_t i = next_index + 1;
               i < blk->first_inst + blk->inst_count; ++i)
            if (mach_inst_uses_vreg(&mach->insts[i], in->src0.as.reg)) {
              src_reused = true;
              break;
            }
          if (preg >= 0 && src_seg && src_seg->carried && mul_const &&
              add_const && !src_reused && next->opcode == NY_MACH_ADD &&
              next->dst.kind == NY_MACH_OPERAND_VREG &&
              next->src0.kind == NY_MACH_OPERAND_VREG &&
              next->src0.as.reg == in->dst.as.reg &&
              consumer->src0.kind == NY_MACH_OPERAND_VREG &&
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
        {
          /*
           * Fold COPY-from-IMM src1 into an immediate operand when the value
           * fits a sign-extended imm32; otherwise use the memory-operand form.
           * Accumulator forms (05/2d/25/0d/35) and imul rax,rax,imm32 (69 c0)
           * both sign-extend imm32 to 64 bits.
           */
          int64_t fold = 0;
          bool fold_ok = false;
          if (in->src1.kind == NY_MACH_OPERAND_VREG && imm_fold_known &&
              in->src1.as.reg < mach->vreg_len &&
              imm_fold_known[in->src1.as.reg]) {
            fold = imm_fold[in->src1.as.reg];
            fold_ok = (int64_t)(int32_t)fold == fold;
          }
          if (fold_ok) {
            unsigned char op = 0;
            if (in->opcode == NY_MACH_ADD)
              op = 0x05; /* add rax, imm32 */
            else if (in->opcode == NY_MACH_SUB)
              op = 0x2d;
            else if (in->opcode == NY_MACH_AND)
              op = 0x25;
            else if (in->opcode == NY_MACH_OR)
              op = 0x0d;
            else if (in->opcode == NY_MACH_XOR)
              op = 0x35;
            if (op) {
              if (!mach_rex(e, true, false, false, false) || !mach_u8(e, op) ||
                  !mach_i32(e, (int32_t)fold) ||
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
          if (in->opcode == NY_MACH_ADD)
            op = 0x03;
          else if (in->opcode == NY_MACH_SUB)
            op = 0x2b;
          else if (in->opcode == NY_MACH_AND)
            op = 0x23;
          else if (in->opcode == NY_MACH_OR)
            op = 0x0b;
          else if (in->opcode == NY_MACH_XOR)
            op = 0x33;
          else {
            if (!mach_rex(e, true, false, false, false) || !mach_u8(e, 0x0f) || !mach_u8(e, 0xaf) ||
                !mach_u8(e, 0x85) || !mach_i32(e, b) ||
                !mach_commit_vreg(e, &in->dst, dst))
              return false;
            break;
          }
          if (!mach_rex(e, true, false, false, false) || !mach_u8(e, op) || !mach_u8(e, 0x85) ||
              !mach_i32(e, b) || !mach_commit_vreg(e, &in->dst, dst))
            return false;
        }
        break;
      case NY_MACH_FMA: {
        /*
         * dst = a*b + c  (mul + add; true FMA when available is a later pass).
         */
        int c_off = mach_slot_off(e, &in->src2);
        if (mach_is_v128(mach, &in->dst)) {
          ny_mach_type_t vt = mach_v128_type(mach, &in->dst);
          if (e->vec_fast_path) {
            if (vt == NY_MACH_TYPE_V128_F64) {
              if (!mach_vector_fma(e, in, dst, 0x66, 0x59, 0x58))
                return false;
            } else if (vt == NY_MACH_TYPE_V128_F32) {
              if (!mach_vector_fma(e, in, dst, 0, 0x59, 0x58))
                return false;
            } else {
              return mach_err(e, "x64 machine form encode: vector FMA type");
            }
            break;
          }
          if (!mach_load_xmm0_v128(e, a))
            return false;
          if (vt == NY_MACH_TYPE_V128_F64) {
            if (!mach_u8(e, 0x66) || !mach_u8(e, 0x0f) || !mach_u8(e, 0x59) ||
                !mach_u8(e, 0x85) || !mach_i32(e, b) || !mach_u8(e, 0x66) ||
                !mach_u8(e, 0x0f) || !mach_u8(e, 0x58) || !mach_u8(e, 0x85) ||
                !mach_i32(e, c_off) || !mach_store_xmm0_v128(e, dst))
              return false;
          } else if (vt == NY_MACH_TYPE_V128_F32) {
            if (!mach_u8(e, 0x0f) || !mach_u8(e, 0x59) || !mach_u8(e, 0x85) ||
                !mach_i32(e, b) || !mach_u8(e, 0x0f) || !mach_u8(e, 0x58) ||
                !mach_u8(e, 0x85) || !mach_i32(e, c_off) ||
                !mach_store_xmm0_v128(e, dst))
              return false;
          } else
            return mach_err(e, "x64 machine form encode: FMA needs f32/f64 v128");
          break;
        }
        if (mach_is_float(mach, &in->dst)) {
          bool f32 = mach_is_f32(mach, &in->dst);
          if (e->fp_fast_path) {
            int dreg = in->dst.kind == NY_MACH_OPERAND_VREG
                           ? mach_vreg_fpreg(e, in->dst.as.reg)
                           : -1;
            if (!mach_float_binop(e, in, dst, f32, 0x59))
              return false;
            if (dreg < 0 && !mach_load_xmm(e, dst, f32, 14))
              return false;
            if (!mach_sse_op_mem_reg(e, dreg >= 0 ? (unsigned)dreg : 14,
                                     c_off, f32, 0x58))
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
      case NY_MACH_SHUFFLE: {
        if (!mach_is_v128(mach, &in->dst) ||
            in->src1.kind != NY_MACH_OPERAND_IMM)
          return mach_err(e, "x64 machine form encode: bad shuffle");
        ny_mach_type_t vt = mach_v128_type(mach, &in->dst);
        unsigned imm = (unsigned)(in->src1.as.imm & 0xff);
        if (e->vec_fast_path) {
          ny_mach_type_t vt = mach_v128_type(mach, &in->dst);
          int assigned = in->dst.kind == NY_MACH_OPERAND_VREG
                             ? mach_vreg_vpreg(e, in->dst.as.reg)
                             : -1;
          unsigned d = assigned >= 0 ? (unsigned)assigned : 14;
          unsigned s = d == 14 ? 15 : 14;
          if (!mach_load_vector_operand(e, &in->src0, s, &s))
            return false;
          if (s != d && !mach_vec_mov_reg(e, d, s))
            return false;
          if (vt == NY_MACH_TYPE_V128_F64) {
            if (!mach_u8(e, 0x66) || !mach_u8(e, 0x0f) || !mach_u8(e, 0xc6) ||
                !mach_u8(e, 0xc0 | ((d & 7) << 3) | (d & 7)) ||
                !mach_u8(e, imm & 3))
              return false;
          } else if (vt == NY_MACH_TYPE_V128_F32) {
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
        if (!mach_load_xmm0_v128(e, a))
          return false;
        if (vt == NY_MACH_TYPE_V128_F64) {
          /*
           * shufpd imm: 66 0F C6 C0 ib
           */
          if (!mach_u8(e, 0x66) || !mach_u8(e, 0x0f) || !mach_u8(e, 0xc6) ||
              !mach_u8(e, 0xc0) || !mach_u8(e, (unsigned char)(imm & 3)) ||
              !mach_store_xmm0_v128(e, dst))
            return false;
        } else if (vt == NY_MACH_TYPE_V128_F32) {
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
      case NY_MACH_MOD:
      mach_div_scalar:
        if (mach_is_v128(mach, &in->dst) && in->opcode == NY_MACH_DIV)
          return mach_err(e, "x64 machine form encode: v128 div handled above");
        if (mach_is_float(mach, &in->dst) && in->opcode == NY_MACH_DIV) {
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
        if (mach_pow2_divisor(&in->src1, imm_fold, imm_fold_known,
                              mach->vreg_len, &pow2_shift)) {
          uint32_t mask = ((uint32_t)1 << pow2_shift) - 1;
          if (!mach_load_op_rax(e, &in->src0) ||
              !mach_rex(e, true, false, false, false) ||
              !mach_u8(e, 0x89) || !mach_u8(e, 0xc1))
            return false; /* mov %rax, %rcx */
          if (in->opcode == NY_MACH_DIV) {
            if (!mach_rex(e, true, false, false, false) ||
                !mach_u8(e, 0xc1) || !mach_u8(e, 0xf8) ||
                !mach_u8(e, 63) ||
                !mach_rex(e, true, false, false, false) ||
                !mach_u8(e, 0x81) || !mach_u8(e, 0xe0) ||
                !mach_i32(e, (int32_t)mask) ||
                !mach_rex(e, true, false, false, false) ||
                !mach_u8(e, 0x01) || !mach_u8(e, 0xc8) ||
                !mach_rex(e, true, false, false, false) ||
                !mach_u8(e, 0xc1) || !mach_u8(e, 0xf8) ||
                !mach_u8(e, pow2_shift) ||
                !mach_commit_vreg(e, &in->dst, dst))
              return false;
          } else {
            if (!mach_rex(e, true, false, false, false) ||
                !mach_u8(e, 0x89) || !mach_u8(e, 0xc2) ||
                !mach_rex(e, true, false, false, false) ||
                !mach_u8(e, 0xc1) || !mach_u8(e, 0xfa) ||
                !mach_u8(e, 63) ||
                !mach_rex(e, true, false, false, false) ||
                !mach_u8(e, 0x81) || !mach_u8(e, 0xe2) ||
                !mach_i32(e, (int32_t)mask) ||
                !mach_rex(e, true, false, false, false) ||
                !mach_u8(e, 0x01) || !mach_u8(e, 0xd0) ||
                !mach_rex(e, true, false, false, false) ||
                !mach_u8(e, 0xc1) || !mach_u8(e, 0xf8) ||
                !mach_u8(e, pow2_shift) ||
                !mach_rex(e, true, false, false, false) ||
                !mach_u8(e, 0xc1) || !mach_u8(e, 0xe0) ||
                !mach_u8(e, pow2_shift) ||
                !mach_rex(e, true, false, false, false) ||
                !mach_u8(e, 0x29) || !mach_u8(e, 0xc1) ||
                !mach_rex(e, true, false, false, false) ||
                !mach_u8(e, 0x89) || !mach_u8(e, 0xc8) ||
                !mach_commit_vreg(e, &in->dst, dst))
              return false;
          }
          break;
        }
        /*
         * Signed 64-bit remainder by 2^31-1 using the exact reciprocal
         * sequence selected by GCC. `imul` produces the high half needed for
         * the quotient; do not replace this with NYIR's low-half MUL.
         */
        int64_t divisor_imm = 0;
        bool mersenne31 =
            in->opcode == NY_MACH_MOD &&
            in->src1.kind == NY_MACH_OPERAND_VREG &&
            in->src1.as.reg < mach->vreg_len &&
            imm_fold_known[in->src1.as.reg] &&
            ((divisor_imm = imm_fold[in->src1.as.reg]) == INT64_C(2147483647));
        if (mersenne31) {
          if (!mach_load_op_rax(e, &in->src0) ||
              /*
               * r9 = dividend; r8 = signed reciprocal.
               */
              !mach_rex(e, true, false, false, true) || !mach_u8(e, 0x89) ||
              !mach_u8(e, 0xc1) ||
              !mach_rex(e, true, false, false, true) || !mach_u8(e, 0xb8) ||
              !mach_i64(e, INT64_C(-9223372032559808509)) ||
              /*
               * rdx:rax = rax * r8; q = (rdx + dividend) >> 30 - sign.
               */
              !mach_rex(e, true, false, false, true) || !mach_u8(e, 0xf7) ||
              !mach_u8(e, 0xe8) ||
              !mach_rex(e, true, true, false, false) || !mach_u8(e, 0x01) ||
              !mach_u8(e, 0xca) ||
              !mach_rex(e, true, false, false, false) || !mach_u8(e, 0xc1) ||
              !mach_u8(e, 0xfa) || !mach_u8(e, 30) ||
              !mach_rex(e, true, true, false, false) || !mach_u8(e, 0x89) ||
              !mach_u8(e, 0xc8) ||
              !mach_rex(e, true, false, false, false) || !mach_u8(e, 0xc1) ||
              !mach_u8(e, 0xf8) || !mach_u8(e, 63) ||
              !mach_rex(e, true, false, false, false) || !mach_u8(e, 0x29) ||
              !mach_u8(e, 0xc2) ||
              /*
               * rax = q * (2^31 - 1); rax = dividend - rax.
               */
              !mach_rex(e, true, false, false, false) || !mach_u8(e, 0x89) ||
              !mach_u8(e, 0xd0) ||
              !mach_rex(e, true, false, false, false) || !mach_u8(e, 0xc1) ||
              !mach_u8(e, 0xe0) || !mach_u8(e, 31) ||
              !mach_rex(e, true, false, false, false) || !mach_u8(e, 0x29) ||
              !mach_u8(e, 0xd0) ||
              !mach_rex(e, true, false, false, true) || !mach_u8(e, 0x29) ||
              !mach_u8(e, 0xc1))
            return false;
          const ny_mach_live_segment_t *dst_seg =
              in->dst.kind == NY_MACH_OPERAND_VREG
                  ? mach_segment_at(e, in->dst.as.reg, e->cur_inst)
                  : NULL;
          if (dst_seg && dst_seg->carried) {
            int preg = mach_vreg_preg(e, in->dst.as.reg);
            if (preg < 0 ||
                !mach_rex(e, true, true, false, preg >= 8) ||
                !mach_u8(e, 0x89) ||
                !mach_u8(e, (unsigned char)(0xc8 | (preg & 7))))
              return false;
          } else if (!mach_rex(e, true, true, false, false) ||
                     !mach_u8(e, 0x89) || !mach_u8(e, 0xc8) ||
                     !mach_commit_vreg(e, &in->dst, dst))
            return false;
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
        if (in->src1.kind == NY_MACH_OPERAND_IMM) {
          sdivisor = in->src1.as.imm;
          sdivisor_known = true;
        } else if (in->src1.kind == NY_MACH_OPERAND_VREG && imm_fold &&
                   imm_fold_known && in->src1.as.reg < mach->vreg_len &&
                   imm_fold_known[in->src1.as.reg]) {
          sdivisor = imm_fold[in->src1.as.reg];
          sdivisor_known = true;
        }
        if (sdivisor_known && sdivisor != 0 && sdivisor != 1 &&
            sdivisor != -1) {
          uint64_t ad = sdivisor >= 0
                            ? (uint64_t)sdivisor
                            : (uint64_t)(-(sdivisor + 1)) + 1;
          if ((ad & (ad - 1)) != 0) {
            int64_t magic = 0;
            unsigned mshift = 0;
            mach_sdiv_magic(ad, &magic, &mshift);
            bool negate = sdivisor < 0 && in->opcode == NY_MACH_DIV;
            if (!mach_load_op_rax(e, &in->src0) ||
                !mach_mov_reg_rax(e, 9) ||
                !mach_rex(e, true, false, false, true) ||
                !mach_u8(e, 0xb8) || !mach_i64(e, magic) ||
                !mach_rex(e, true, false, false, true) ||
                !mach_u8(e, 0xf7) || !mach_u8(e, 0xe8) ||
                !mach_mov_rax_reg(e, 2))
              return false;
            if (magic < 0 && !mach_alu_preg_preg(e, NY_MACH_ADD, 0, 9))
              return false;
            if (mshift > 0 &&
                (!mach_rex(e, true, false, false, false) ||
                 !mach_u8(e, 0xc1) || !mach_u8(e, 0xf8) ||
                 !mach_u8(e, (unsigned char)mshift)))
              return false;
            if (!mach_mov_preg_preg(e, 1, 9) ||
                !mach_rex(e, true, false, false, false) ||
                !mach_u8(e, 0xc1) || !mach_u8(e, 0xf9) ||
                !mach_u8(e, 63) ||
                !mach_alu_preg_preg(e, NY_MACH_SUB, 0, 1))
              return false;
            if (negate &&
                (!mach_rex(e, true, false, false, false) ||
                 !mach_u8(e, 0xf7) || !mach_u8(e, 0xd8)))
              return false;
            if (in->opcode == NY_MACH_MOD) {
              if (!mach_mov_reg_rax(e, 8) || !mach_mov_rax_reg(e, 9))
                return false;
              if ((int64_t)(int32_t)ad == (int64_t)ad) {
                if (!mach_alu_preg_imm(e, NY_MACH_MUL, 8, (int64_t)ad))
                  return false;
              } else if (!mach_rex(e, true, false, false, true) ||
                         !mach_u8(e, 0xba) || !mach_i64(e, (int64_t)ad) ||
                         !mach_alu_preg_preg(e, NY_MACH_MUL, 8, 10)) {
                return false;
              }
              if (!mach_alu_preg_preg(e, NY_MACH_SUB, 0, 8))
                return false;
            }
            if (!mach_commit_vreg(e, &in->dst, dst))
              return false;
            break;
          }
        }
        /*
         * `a` may be a colored vreg.  Seed it through the allocator-aware
         * operand loader; loading its home directly reads stale data when the
         * preceding arithmetic intentionally kept the value resident.  The
         * divisor remains a memory operand and therefore has a canonical home.
         */
        if (!mach_load_op_rax(e, &in->src0) ||
            !mach_rex(e, true, false, false, false) || !mach_u8(e, 0x99) ||
            !mach_rex(e, true, false, false, false) || !mach_u8(e, 0xf7) ||
            !mach_u8(e, 0xbd) || !mach_i32(e, b))
          return false;
        if (in->opcode == NY_MACH_MOD) {
          /*
           * mov %rdx, %rax then dual-write commit
           */
          if (!mach_rex(e, true, false, false, false) || !mach_u8(e, 0x89) || !mach_u8(e, 0xd0))
            return false;
          if (!mach_commit_vreg(e, &in->dst, dst))
            return false;
        } else if (!mach_commit_vreg(e, &in->dst, dst))
          return false;
        break;
      case NY_MACH_CMP: {
        /*
         * Flags-fused compare: if this compare's result is consumed only by
         * an immediately following BR_IF in the same block, keep RFLAGS from
         * the cmp and let the branch emit jcc directly.  setcc, movzbq, and
         * stores never modify flags, so they stay live through the old
         * materialization.  The cond vreg must die at the branch (segment
         * ends there) and must not be live-out, so nothing can later reseed
         * the stale home.
         */
        bool fuse_branch = false;
        if (in->dst.kind == NY_MACH_OPERAND_VREG &&
            e->cur_inst + 1 < mach->inst_len &&
            e->cur_inst + 1 < blk->first_inst + blk->inst_count) {
          const ny_mach_inst_t *fnext = &mach->insts[e->cur_inst + 1];
          if (fnext->opcode == NY_MACH_BR_IF &&
              fnext->src0.kind == NY_MACH_OPERAND_VREG &&
              fnext->src0.as.reg == in->dst.as.reg) {
            const ny_mach_live_segment_t *fseg =
                mach_segment_at(e, in->dst.as.reg, e->cur_inst);
            if (fseg && fseg->end == e->cur_inst + 1 &&
                !ny_mach_regalloc_live_out(&e->regalloc, bi,
                                           in->dst.as.reg))
              fuse_branch = true;
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
              !mach_rex(e, true, false, false, false) || !mach_u8(e, 0x0f) || !mach_u8(e, 0xb6) ||
              !mach_u8(e, 0xc0) || !mach_store_rax(e, dst))
            return false;
          break;
        }
        if (!mach_is_i64(mach, &in->src0))
          return mach_err(e, "x64 machine form encode: unsupported cmp type");
        if (!mach_load_op_rax(e, &in->src0))
          return false;
        {
          /*
           * cmp rax, imm32 (3d, sign-extended) when src1 is a known constant.
           */
          int64_t fold = 0;
          bool fold_ok = false;
          if (in->src1.kind == NY_MACH_OPERAND_IMM) {
            fold = in->src1.as.imm;
            fold_ok = (int64_t)(int32_t)fold == fold;
          } else if (in->src1.kind == NY_MACH_OPERAND_VREG && imm_fold_known &&
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
            if (in->src1.kind == NY_MACH_OPERAND_VREG) {
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
            !mach_rex(e, true, false, false, false) || !mach_u8(e, 0x0f) || !mach_u8(e, 0xb6) ||
            !mach_u8(e, 0xc0) || !mach_commit_vreg(e, &in->dst, dst))
          return false;
        break;
      }
      case NY_MACH_BR:
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
        if (!mach_u8(e, 0xe9) || !mach_add_patch(e, in->src1.as.block_index) ||
            !mach_i32(e, 0))
          return false;
        break;
      case NY_MACH_BR_IF:
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
        /*
         * cmpq $0, mem; jne rel32
         */
        if (!mach_rex(e, true, false, false, false) || !mach_u8(e, 0x83) || !mach_u8(e, 0xbd) ||
            !mach_i32(e, a) || !mach_u8(e, 0x00) || !mach_u8(e, 0x0f) ||
            !mach_u8(e, 0x85) || !mach_add_patch(e, in->src1.as.block_index) ||
            !mach_i32(e, 0))
          return false;
        break;
      case NY_MACH_CALL: {
        if (in->src0.kind != NY_MACH_OPERAND_SYMBOL || !in->src0.as.symbol)
          return mach_err(e, "x64 machine form encode: call needs symbol");
        if (!mach_flush_float_to_home(e) || !mach_flush_vector_to_home(e))
          return false;
        /*
         * SysV: up to 6 GP + 8 SSE; aggregates ≤16B split per arg_sizes class.
         */
        bool is_win = e->target && e->target->abi == NY_NATIVE_ABI_WIN64;
        static const int gp_sysv[] = {7, 6, 2, 1, 8, 9}; /* rdi rsi rdx rcx r8 r9 */
        static const int gp_win[] = {1, 2, 8, 9};
        const int *gp = is_win ? gp_win : gp_sysv;
        size_t max_gp = is_win ? 4 : 6;
        size_t max_fp = is_win ? 4 : 8;
        size_t gp_n = 0, fp_n = 0, ordinal_n = 0, stack_n = 0;
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
            ++ordinal_n;
          } else if (is_fp) {
            if (fp_n < max_fp)
              ++fp_n;
            else
              ++stack_n;
          } else if (gp_n < max_gp)
            ++gp_n;
          else
            ++stack_n;
        }
        size_t shadow = is_win && e->target ? e->target->shadow_space_bytes : 0;
        size_t stack_bytes = ((stack_n * 8 + shadow) + 15) & ~(size_t)15;
        if (stack_bytes) {
          if (!mach_rex(e, true, false, false, false) || !mach_u8(e, 0x81) ||
              !mach_u8(e, 0xec) ||
              !mach_i32(e, (int32_t)stack_bytes))
            return false;
        }
        gp_n = fp_n = ordinal_n = 0;
        size_t stack_i = 0;
        for (size_t ai = 0; ai < in->args_len; ++ai) {
          int aoff = mach_slot_off(e, &in->args[ai]);
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
                  unsigned char modrm =
                      (unsigned char)(0x40 | ((xn & 7) << 3));
                  if (!mach_u8(e, 0xf2) || !mach_u8(e, 0x0f) || !mach_u8(e, 0x10) ||
                      !mach_u8(e, modrm) || !mach_u8(e, (unsigned)(ch * 8)))
                    return false;
                }
              }
            } else if (agg_mem[ai]) {
              /*
               * MEMORY by-value: memcpy aggregate onto stack arg area.
               */
              uint32_t size = agg_size[ai];
              size_t slots = (size + 7) / 8;
              if (!mach_load_op_rax(e, &in->args[ai]) || !mach_rex(e, true, false, false, false) ||
                  !mach_u8(e, 0x89) || !mach_u8(e, 0xc6) || /* mov %rax,%rsi */
                  !mach_rex(e, true, false, false, false) || !mach_u8(e, 0x8d) || !mach_u8(e, 0xbc) ||
                  !mach_u8(e, 0x24) ||
                  !mach_i32(e, (int32_t)(shadow + stack_i * 8)) || /* lea disp(%rsp),%rdi */
                  !mach_rex(e, true, false, false, false) || !mach_u8(e, 0xc7) || !mach_u8(e, 0xc1) ||
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
          bool on_stack = is_win ? ordinal_n >= max_gp
                                 : is_fp ? fp_n >= max_fp : gp_n >= max_gp;
          if (on_stack) {
            int stack_off = (int)(shadow + stack_i * 8);
            if (is_f32) {
              if (!mach_load_eax(e, aoff) || !mach_store_eax_rsp(e, stack_off))
                return false;
            } else if (!mach_load_op_rax(e, &in->args[ai]) || !mach_rex(e, true, false, false, false) ||
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
            if (!mach_load_op_rax(e, &in->args[ai]) ||
                !mach_mov_reg_rax(e, reg))
              return false;
            if (!is_win)
              ++gp_n;
          }
          if (is_win)
            ++ordinal_n;
        }
        char call_sym[256];
        const char *raw = in->src0.as.symbol;
        const char *pref =
            e->target && e->target->symbol_prefix ? e->target->symbol_prefix
                                                  : "";
        if (in->call_is_extern || strncmp(raw, "rt_", 3) == 0 ||
            strncmp(raw, "ny_fn_", 6) == 0 || raw[0] == '_')
          snprintf(call_sym, sizeof(call_sym), "%s%s", pref, raw);
        else
          snprintf(call_sym, sizeof(call_sym), NY_FMT_FN, pref, raw);
        if (!mach_u8(e, 0xe8) ||
            !mach_add_reloc(e, call_sym, NY_RELOC_PLT32) || !mach_i32(e, 0))
          return false;
        if (stack_bytes) {
          if (!mach_rex(e, true, false, false, false) || !mach_u8(e, 0x81) || !mach_u8(e, 0xc4) ||
              !mach_i32(e, (int32_t)stack_bytes))
            return false;
        }
        mach_rax_invalidate(e);
        if (in->dst.kind == NY_MACH_OPERAND_VREG) {
          if (mach_is_float(mach, &in->dst)) {
            bool f32 = mach_is_f32(mach, &in->dst);
            int preg = e->fp_fast_path
                           ? mach_vreg_fpreg(e, in->dst.as.reg)
                           : -1;
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
      case NY_MACH_RET:
        if (in->src0.kind != NY_MACH_OPERAND_NONE) {
          if (mach_is_float(mach, &in->src0)) {
            bool f32 = mach_is_f32(mach, &in->src0);
            int preg = in->src0.kind == NY_MACH_OPERAND_VREG
                           ? mach_vreg_fpreg(e, in->src0.as.reg)
                           : -1;
            if (e->fp_fast_path && preg >= 0) {
              if (!mach_seed_float_preg(e, in->src0.as.reg) ||
                  (preg != 0 && !mach_sse_mov_reg(e, 0, (unsigned)preg, f32)))
                return false;
            } else if (!mach_load_xmm0(e, mach_slot_off(e, &in->src0), f32))
              return false;
          } else if (!mach_load_op_rax(e, &in->src0))
            return false;
        }
        /*
         * Epilogue: add frame; pop callee-saves; pop rbp; ret.
         * Do not use leave after pops — leave would reset %rsp to %rbp and
         * undo the callee-save pops.
         */
        if (frame) {
          if (!mach_rex(e, true, false, false, false) || !mach_u8(e, 0x81) || !mach_u8(e, 0xc4) ||
              !mach_i32(e, frame))
            return false;
        }
        if (!mach_pop_callee_saves(e, save_mask))
          return false;
        if (!mach_u8(e, 0x5d) || !mach_u8(e, 0xc3)) /* pop %rbp; ret */
          return false;
        break;
      case NY_MACH_NOP:
        break;
      case NY_MACH_TRAP:
        /*
         * ud2 — raises #UD (invalid opcode), terminating the process.
         */
        if (!mach_u8(e, 0x0f) || !mach_u8(e, 0x0b))
          return false;
        break;
      case NY_MACH_INLINE_ASM:
        /*
         * Encode only templates with a defined x86-64 lowering.  An
         * unrecognised template is an owning encoder error, not a nop:
         * silently discarding inline assembly changes program semantics.
         */
        if (in->src0.kind != NY_MACH_OPERAND_SYMBOL || !in->src0.as.symbol)
          return mach_err(e, "x64 machine form encode: inline asm needs template");
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
              if ((p == t || p[-1] == ';') &&
                  (p[0] == 'n' || p[0] == 'N')) {
                if (!mach_u8(e, 0x90))
                  return false;
                emitted = true;
              }
            }
            if (!emitted)
              return mach_err(e, "x64 machine form encode: unsupported inline asm template");
          } else {
            return mach_err(e, "x64 machine form encode: unsupported inline asm template");
          }
        }
        break;
      case NY_MACH_REDUCE_ADD:
        if (mach_is_f64(mach, &in->dst)) {
          if (!mach_reduce_add_f64(e, in, dst))
            return false;
        } else if (!mach_reduce_add_i64(e, in, dst)) {
          return false;
        }
        break;
      case NY_MACH_INTRINSIC: {
        /*
         * COPY_STRUCT: src0=dst-ptr, src1=src-ptr, src2=IMM size.
         * CAPTURE_RET: dst=vreg, src0=IMM selector (0=rdx,1=rax,2=xmm0,3=xmm1).
         */
        if (in->src2.kind == NY_MACH_OPERAND_IMM &&
            in->src0.kind == NY_MACH_OPERAND_VREG) {
          int64_t nbytes = in->src2.as.imm;
          if (nbytes <= 0)
            break;
          int dstp = mach_slot_off(e, &in->src0);
          int srcp = mach_slot_off(e, &in->src1);
          if (!mach_load_rax(e, srcp) || !mach_rex(e, true, false, false, false) || !mach_u8(e, 0x89) ||
              !mach_u8(e, 0xc6) ||
              !mach_load_rax(e, dstp) || !mach_rex(e, true, false, false, false) || !mach_u8(e, 0x89) ||
              !mach_u8(e, 0xc7) ||
              !mach_rex(e, true, false, false, false) || !mach_u8(e, 0xc7) || !mach_u8(e, 0xc1) ||
              !mach_i32(e, (int32_t)nbytes) ||
              !mach_u8(e, 0xf3) || !mach_u8(e, 0xa4))
            return false;
          mach_rax_invalidate(e);
          break;
        }
        if (in->src0.kind != NY_MACH_OPERAND_IMM)
          return mach_err(e, "x64 machine form encode: bad intrinsic shape");
        unsigned sel = (unsigned)(in->src0.as.imm & 0xff);
        if (sel > 3)
          return mach_err(e, "x64 machine form encode: bad capture.ret selector");
        if (sel == 0) {
          if (!mach_rex(e, true, false, false, false) || !mach_u8(e, 0x89) || !mach_u8(e, 0xd0))
            return false;
        } else if (sel == 2) {
          if (!mach_u8(e, 0x66) || !mach_rex(e, true, false, false, false) || !mach_u8(e, 0x0f) ||
              !mach_u8(e, 0x7e) || !mach_u8(e, 0xc0))
            return false;
        } else if (sel == 3) {
          if (!mach_u8(e, 0x66) || !mach_rex(e, true, false, false, false) || !mach_u8(e, 0x0f) ||
              !mach_u8(e, 0x7e) || !mach_u8(e, 0xc8))
            return false;
        }
        /*
         * sel==1: rax already primary return
         */
        mach_rax_invalidate(e);
        if (in->dst.kind == NY_MACH_OPERAND_VREG) {
          if (!mach_commit_vreg(e, &in->dst, mach_slot_off(e, &in->dst)))
            return false;
        }
        break;
      }
      default:
        return mach_err(e, "x64 machine form encode: unsupported opcode (NYIR fallback)");
      }
      if (in->dst.kind == NY_MACH_OPERAND_VREG && e->color_seeded &&
          in->dst.as.reg < e->colors_len &&
          mach_vreg_preg(e, in->dst.as.reg) >= 0)
        e->color_seeded[in->dst.as.reg] = true;
      if (!mach_end_segments(e))
        return false;
      if (!mach_float_end_segments(e))
        return false;
      if (!mach_vector_end_segments(e))
        return false;
      if (e->pending_rax_valid) {
        e->last_rax_valid = true;
        e->last_rax_off = e->pending_rax_off;
      }
    }
    if (!mach_save_live_out(e, bi))
      return false;
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
  size_t gpr_peak_live = ny_mach_regalloc_peak_live(&e->regalloc, e->mach->inst_len);
  ny_native_mach_regalloc_record(e->regalloc.segment_len, colored_segments,
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
      ny_mach_regalloc_peak_live(&e->fp_regalloc, e->mach->inst_len);
  ny_native_mach_fpr_record(e->fp_regalloc.segment_len, fp_colored_segments,
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
      ny_mach_regalloc_peak_live(&e->vec_regalloc, e->mach->inst_len);
  ny_native_mach_vector_record(e->vec_regalloc.segment_len,
                               vec_colored_segments, vec_spilled_segments,
                               vec_reload_segments, vec_peak_live);
  ny_mach_regalloc_free(&e->vec_regalloc);
  ny_mach_regalloc_free(&e->regalloc);
  ny_mach_regalloc_free(&e->fp_regalloc);
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
  ny_obj_buf_t code;
  ny_x64_obj_reloc_t *relocs;
  size_t reloc_count;
  char symbol[256];
  char error[256];
} ny_x64_parallel_encode_result_t;

typedef struct {
  const ny_mach_func_t *funcs;
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
  ny_mach_func_t mach_mut = ctx->funcs[i];
  (void)ny_isle_apply_mach(&mach_mut);
  ny_x64_mach_enc_t enc = {0};
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

bool ny_x64_mach_append_function(
    ny_obj_buf_t *code, ny_x64_obj_symbol_def_t *defs, size_t *def_count,
    ny_x64_obj_reloc_t *relocs, size_t *reloc_count,
    const ny_mach_func_t *mach, const ny_native_target_info_t *target,
    const char *symbol, bool tag_return, char *err, size_t err_len) {
  if (!code || !defs || !def_count || !relocs || !reloc_count || !mach ||
      !target || !symbol || !symbol[0]) {
    ny_native_set_err(err, err_len,
                      "x86-64 machine form append: missing input");
    return false;
  }
  ny_mach_func_t mach_mut = *mach;
  if (getenv("NY_DUMP_MACH"))
    ny_mach_dump(mach, stderr);
  (void)ny_isle_apply_mach(&mach_mut);
  if (getenv("NY_DUMP_MACH"))
    ny_mach_dump(&mach_mut, stderr);
  ny_x64_mach_enc_t enc = {0};
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
  mach_free_remat_maps(&enc);
  if (*def_count >= NY_NATIVE_MAX_DEFS ||
      !ny_obj_emit(code, enc.code.data, enc.code.len)) {
    ny_native_set_err(err, err_len,
                      "x86-64 machine form append: object buffer is full");
    free(enc.code.data);
    free(enc.relocs);
    free(enc.block_off);
    free(enc.patch_at);
    free(enc.patch_block);
    return false;
  }
  size_t start = code->len - enc.code.len;
  snprintf(defs[*def_count].name, sizeof(defs[*def_count].name), "%s", symbol);
  defs[*def_count].off = start;
  defs[*def_count].size = enc.code.len;
  (*def_count)++;
  for (size_t i = 0; i < enc.reloc_count; ++i) {
    if (*reloc_count >= NY_X64_OBJ_MAX_RELOCS) {
      ny_native_set_err(err, err_len,
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

bool ny_x64_mach_build_bundle(
    const ny_mach_func_t *rt_main_mir, const ny_mach_func_t *func_mirs,
    const char *const *func_names, size_t func_count,
    const ny_native_target_info_t *target, const char *entry_symbol,
    bool tag_return, ny_obj_buf_t *code, ny_x64_obj_symbol_def_t *defs,
    size_t *def_count, ny_x64_obj_reloc_t *relocs, size_t *reloc_count,
    char *err, size_t err_len) {
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
        ny_obj_free(&results[i].code);
        free(results[i].relocs);
      }
      free(order);
      free(results);
      return false;
    }
    for (size_t oi = 0; oi < func_count; ++oi) {
      ny_x64_parallel_encode_result_t *r = &results[oi];
      size_t start = code->len;
      if (*def_count >= NY_NATIVE_MAX_DEFS ||
          !ny_obj_emit(code, r->code.data, r->code.len)) {
        for (size_t j = oi; j < func_count; ++j) {
          ny_obj_free(&results[j].code);
          free(results[j].relocs);
        }
        free(order);
        free(results);
        return false;
      }
      snprintf(defs[*def_count].name, sizeof(defs[*def_count].name), "%s",
               r->symbol);
      defs[*def_count].off = start;
      defs[*def_count].size = r->code.len;
      (*def_count)++;
      for (size_t j = 0; j < r->reloc_count; ++j) {
        if (*reloc_count >= NY_X64_OBJ_MAX_RELOCS) {
          for (size_t k = oi; k < func_count; ++k) {
            ny_obj_free(&results[k].code);
            free(results[k].relocs);
          }
          free(order);
          free(results);
          return false;
        }
        relocs[*reloc_count] = r->relocs[j];
        relocs[*reloc_count].disp_off += start;
        (*reloc_count)++;
      }
      ny_obj_free(&r->code);
      free(r->relocs);
      r->relocs = NULL;
    }
  }
  free(order);
  free(results);

  char entry[256];
  snprintf(entry, sizeof(entry), "%s%s",
           target->symbol_prefix ? target->symbol_prefix : "", entry_symbol);
  ny_mach_func_t main_mut = *rt_main_mir;
  (void)ny_isle_apply_mach(&main_mut);
  ny_x64_mach_enc_t enc = {0};
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
  if (!ny_obj_emit(code, enc.code.data, enc.code.len) || *def_count >= NY_NATIVE_MAX_DEFS) {
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
  if (!ny_native_strtab_append_defs(code, defs, def_count, err, err_len))
    return false;
  if (!ny_native_consttab_append_defs(code, defs, def_count, err, err_len))
    return false;
  return ny_native_arraytab_append_defs(code, defs, def_count, err, err_len);
}

/*
 * Copy-and-patch style stencils for the absolute smallest programs.
 * Template bytes with holes patched for immediates only — no full RA.
 * Covers: const ret, and a measured fast path for pure foldable i64.
 */

static bool ny_x64_stencil_const_i64(ny_obj_buf_t *code, int64_t imm,
                              ny_x64_obj_symbol_def_t *defs, size_t *def_count,
                              const char *symbol) {
  if (!code || !defs || !def_count || !symbol)
    return false;
  size_t start = code->len;
  /*
   * push rbp; mov rbp,rsp; movabs rax,imm; leave; ret
   */
  unsigned char prolog[] = {0x55, 0x48, 0x89, 0xe5, 0x48, 0xb8};
  if (!ny_obj_emit(code, prolog, sizeof(prolog)))
    return false;
  for (int i = 0; i < 8; ++i) {
    unsigned char b = (unsigned char)(((uint64_t)imm >> (i * 8)) & 0xff);
    if (!ny_obj_emit(code, &b, 1))
      return false;
  }
  unsigned char ep[] = {0xc9, 0xc3};
  if (!ny_obj_emit(code, ep, sizeof(ep)))
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
static bool ny_x64_stencil_local_i64(ny_obj_buf_t *code, int64_t imm,
                              ny_x64_obj_symbol_def_t *defs, size_t *def_count,
                              const char *symbol) {
  if (!code || !defs || !def_count || !symbol)
    return false;
  size_t start = code->len;
  /*
   * push rbp; mov rbp,rsp; sub rsp,16; movabs rax,imm; mov [rbp-8],rax;
   * mov rax,[rbp-8]; leave; ret
   */
  unsigned char bytes[] = {
      0x55, 0x48, 0x89, 0xe5, 0x48, 0x83, 0xec, 0x10, 0x48, 0xb8};
  if (!ny_obj_emit(code, bytes, sizeof(bytes)))
    return false;
  for (int i = 0; i < 8; ++i) {
    unsigned char b = (unsigned char)(((uint64_t)imm >> (i * 8)) & 0xff);
    if (!ny_obj_emit(code, &b, 1))
      return false;
  }
  unsigned char mid[] = {0x48, 0x89, 0x45, 0xf8, 0x48, 0x8b, 0x45, 0xf8, 0xc9,
                         0xc3};
  if (!ny_obj_emit(code, mid, sizeof(mid)))
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
 * Fold a pure helper body with known integer args into a single result.
 * Rejects mem/extern/PHI/indirect. Returns false if not fully foldable.
 */
static bool ny_x64_stencil_fold_helper(const nyir_func_t *fn, const int64_t *args,
                                       int argc, int64_t *out) {
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
    const nyir_inst_t *in = &fn->data[pc];
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
      case NYIR_ADD_I64: r = a + b; break;
      case NYIR_SUB_I64: r = a - b; break;
      case NYIR_MUL_I64: r = a * b; break;
      case NYIR_AND_I64: r = a & b; break;
      case NYIR_OR_I64: r = a | b; break;
      case NYIR_XOR_I64: r = a ^ b; break;
      case NYIR_SHL_I64: r = a << (b & 63); break;
      case NYIR_SAR_I64: r = a >> (b & 63); break;
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
        case NYIR_CMP_EQ: r = a == b; break;
        case NYIR_CMP_NE: r = a != b; break;
        case NYIR_CMP_LT: r = a < b; break;
        case NYIR_CMP_LE: r = a <= b; break;
        case NYIR_CMP_GT: r = a > b; break;
        case NYIR_CMP_GE: r = a >= b; break;
        default: okf = false; break;
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
    const nyir_func_t *rt_main, const nyir_func_t *funcs,
    const char *const *func_names, size_t func_count,
    const ny_native_target_info_t *target, ny_obj_buf_t *code,
    ny_x64_obj_symbol_def_t *defs, size_t *def_count, ny_x64_obj_reloc_t *relocs,
    size_t *reloc_count, char *err, size_t err_len) {
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
    nyir_op_t op = rt_main->data[i].op;
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
    if (rt_main->data[i].op == NYIR_LABEL &&
        rt_main->data[i].imm >= 0 &&
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
    const nyir_inst_t *in = &rt_main->data[pc];
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
      case NYIR_ADD_I64: r = a + b; break;
      case NYIR_SUB_I64: r = a - b; break;
      case NYIR_MUL_I64: r = a * b; break;
      case NYIR_AND_I64: r = a & b; break;
      case NYIR_OR_I64: r = a | b; break;
      case NYIR_XOR_I64: r = a ^ b; break;
      case NYIR_SHL_I64: r = a << (b & 63); break;
      case NYIR_SAR_I64: r = a >> (b & 63); break;
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
        case NYIR_CMP_EQ: r = a == b; break;
        case NYIR_CMP_NE: r = a != b; break;
        case NYIR_CMP_LT: r = a < b; break;
        case NYIR_CMP_LE: r = a <= b; break;
        case NYIR_CMP_GT: r = a > b; break;
        case NYIR_CMP_GE: r = a >= b; break;
        default: okf = false; break;
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
    if (in->op == NYIR_CALL && allow_calls && in->symbol) {
      /*
       * Resolve helper by name; fold with known args.
       */
      const nyir_func_t *callee = NULL;
      for (size_t fi = 0; fi < func_count; ++fi) {
        if (func_names[fi] && strcmp(func_names[fi], in->symbol) == 0) {
          callee = &funcs[fi];
          break;
        }
      }
      if (!callee)
        break;
      int carg[8];
      int cargc = 0;
      if (!nyir_call_args(in, rt_main->next_value, carg, 8, &cargc, NULL, 0))
        break;
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
        break;
      int64_t r = 0;
      if (!ny_x64_stencil_fold_helper(callee, avals, cargc, &r))
        break;
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
      bool ok =
          used_local
              ? ny_x64_stencil_local_i64(code, val[in->a], defs, def_count, entry)
              : ny_x64_stencil_const_i64(code, val[in->a], defs, def_count,
                                         entry);
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
}

bool ny_x64_try_stencil_bundle(const nyir_func_t *rt_main,
                               const ny_native_target_info_t *target,
                               ny_obj_buf_t *code, ny_x64_obj_symbol_def_t *defs,
                               size_t *def_count, ny_x64_obj_reloc_t *relocs,
                               size_t *reloc_count, char *err, size_t err_len) {
  return ny_x64_try_stencil_bundle_impl(rt_main, NULL, NULL, 0, target, code,
                                        defs, def_count, relocs, reloc_count,
                                        err, err_len);
}

bool ny_x64_try_stencil_bundle_calls(
    const nyir_func_t *rt_main, const nyir_func_t *funcs,
    const char *const *func_names, size_t func_count,
    const ny_native_target_info_t *target, ny_obj_buf_t *code,
    ny_x64_obj_symbol_def_t *defs, size_t *def_count, ny_x64_obj_reloc_t *relocs,
    size_t *reloc_count, char *err, size_t err_len) {
  return ny_x64_try_stencil_bundle_impl(rt_main, funcs, func_names, func_count,
                                        target, code, defs, def_count, relocs,
                                        reloc_count, err, err_len);
}
