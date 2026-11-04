/*
 * A unique CFG edge may carry a live value across non-adjacent block layout.
 */
#include "code/backend/ir/machine.h"

#include <stdio.h>

static nyr_mach_operand_t vreg(nyr_mach_reg_t reg) {
  return (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_VREG,
                              .reg_class = NYR_MACH_REGCLASS_GPR,
                              .as.reg = reg};
}

static nyr_mach_operand_t block(size_t index) {
  return (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_BLOCK,
                              .as.block_index = (uint32_t)index};
}

static bool emit(nyr_mach_func_t *mach, nyr_mach_opcode_t opcode,
                 nyr_mach_operand_t dst, nyr_mach_operand_t src0,
                 nyr_mach_operand_t src1) {
  return nyr_mach_emit(
      mach, (nyr_mach_inst_t){
                .opcode = opcode,
                .dst = dst,
                .src0 = src0,
                .src1 = src1,
                .effects = (opcode == NYR_MACH_BR || opcode == NYR_MACH_RET)
                               ? NYR_MACH_EFFECT_CONTROL
                               : NYR_MACH_EFFECT_NONE});
}

int main(void) {
  nyr_mach_func_t mach = {0};
  nyr_mach_regalloc_t alloc = {0};
  nyr_mach_reg_t value, other_value, edge_copy;
  uint32_t b0, b1, b2;
  char err[256] = {0};
  bool ok =
      nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &value) &&
      nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &other_value) &&
      nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &edge_copy) &&
      nyr_mach_begin_block(&mach, 0, &b0) && b0 == 0 &&
      emit(&mach, NYR_MACH_COPY, vreg(value),
           (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_IMM, .as.imm = 42},
           (nyr_mach_operand_t){0}) &&
      emit(&mach, NYR_MACH_COPY, vreg(other_value),
           (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_IMM, .as.imm = 7},
           (nyr_mach_operand_t){0}) &&
      emit(&mach, NYR_MACH_BR, (nyr_mach_operand_t){0}, (nyr_mach_operand_t){0},
           block(2)) &&
      nyr_mach_begin_block(&mach, 1, &b1) && b1 == 1 &&
      emit(&mach, NYR_MACH_RET, (nyr_mach_operand_t){0}, vreg(other_value),
           (nyr_mach_operand_t){0}) &&
      nyr_mach_begin_block(&mach, 2, &b2) && b2 == 2 &&
      emit(&mach, NYR_MACH_COPY, vreg(edge_copy), vreg(value),
           (nyr_mach_operand_t){0}) &&
      emit(&mach, NYR_MACH_BR, (nyr_mach_operand_t){0}, (nyr_mach_operand_t){0},
           block(1)) &&
      nyr_mach_verify(&mach, 0, err, sizeof(err)) &&
      nyr_mach_regalloc_build_callee_saved_gpr(&mach, 5, &alloc);

  const nyr_mach_live_segment_t *from =
      ok ? nyr_mach_regalloc_segment_at(&alloc, value, 1) : NULL;
  const nyr_mach_live_segment_t *to =
      ok ? nyr_mach_regalloc_segment_at(&alloc, value, 4) : NULL;
  const nyr_mach_live_segment_t *other_in_target =
      ok ? nyr_mach_regalloc_segment_at(&alloc, other_value, 4) : NULL;
  ok = ok && from && to && from->color >= 0 && from->color == to->color &&
       !from->spill && !to->spill && !to->reload && to->carried &&
       other_in_target;
  if (ok) {
    nyr_mach_live_segment_t *mutable_from = NULL;
    nyr_mach_live_segment_t *mutable_to = NULL;
    for (size_t i = 0; i < alloc.segment_len; ++i) {
      nyr_mach_live_segment_t *seg = &alloc.segments[i];
      if (seg->vreg == value && seg->block == b0)
        mutable_from = seg;
      if (seg->vreg == value && seg->block == b2)
        mutable_to = seg;
    }
    if (!mutable_from || !mutable_to) {
      ok = false;
    } else {
      int saved_from_color = mutable_from->color;
      mutable_from->color = (saved_from_color + 1) % (int)alloc.color_count;
      bool accepted_wrong_incoming_color =
          nyr_mach_regalloc_verify(&mach, &alloc, NYR_MACH_REGCLASS_GPR);
      mutable_from->color = saved_from_color;
      ok = !accepted_wrong_incoming_color &&
           nyr_mach_regalloc_verify(&mach, &alloc, NYR_MACH_REGCLASS_GPR);
      if (ok && mutable_to->start < mach.inst_len - 1) {
        size_t saved_start = mutable_to->start;
        size_t saved_end = mutable_to->end;
        mutable_to->start++;
        mutable_to->end++;
        bool accepted_missing_live_in =
            nyr_mach_regalloc_verify(&mach, &alloc, NYR_MACH_REGCLASS_GPR);
        mutable_to->start = saved_start;
        mutable_to->end = saved_end;
        ok = !accepted_missing_live_in &&
             nyr_mach_regalloc_verify(&mach, &alloc, NYR_MACH_REGCLASS_GPR);
      } else {
        ok = false;
      }
      if (ok && mutable_from->end > mutable_from->start) {
        size_t saved_end = mutable_from->end;
        mutable_from->end--;
        bool accepted_missing_live_out =
            nyr_mach_regalloc_verify(&mach, &alloc, NYR_MACH_REGCLASS_GPR);
        mutable_from->end = saved_end;
        ok = !accepted_missing_live_out &&
             nyr_mach_regalloc_verify(&mach, &alloc, NYR_MACH_REGCLASS_GPR);
      } else {
        ok = false;
      }
    }
  }
  if (!ok)
    fprintf(stderr,
            "[regalloc-cfg-edge-carry] unique edge failed to carry value: "
            "from=%p(%d,%d,%d,%d) to=%p(%d,%d,%d,%d) "
            "other=%p(%d,%d,%d) error=%s\n",
            (const void *)from, from ? from->color : -2,
            from ? from->spill : -1, from ? from->reload : -1,
            from ? from->carried : -1, (const void *)to, to ? to->color : -2,
            to ? to->spill : -1, to ? to->reload : -1, to ? to->carried : -1,
            (const void *)other_in_target,
            other_in_target ? other_in_target->color : -2,
            other_in_target ? other_in_target->spill : -1,
            other_in_target ? other_in_target->reload : -1,
            err[0] ? err : "spill/reload contract not coalesced");
  else
    puts("[regalloc-cfg-edge-carry] non-adjacent CFG carry verified; mismatched incoming color rejected");
  nyr_mach_regalloc_free(&alloc);
  nyr_mach_func_free(&mach);
  return ok ? 0 : 1;
}
