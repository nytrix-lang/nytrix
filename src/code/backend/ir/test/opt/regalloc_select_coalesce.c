/*
 * A SELECT result may reuse an arm's color when that arm dies at the SELECT.
 */
#include "code/backend/ir/machine.h"

#include <stdio.h>

static nyr_mach_operand_t vreg(nyr_mach_reg_t reg) {
  return (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_VREG,
                              .reg_class = NYR_MACH_REGCLASS_GPR,
                              .as.reg = reg};
}

static nyr_mach_operand_t imm(int64_t value) {
  return (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_IMM, .as.imm = value};
}

static bool emit_copy(nyr_mach_func_t *mach, nyr_mach_reg_t dst,
                      nyr_mach_operand_t src) {
  return nyr_mach_emit(mach, (nyr_mach_inst_t){
                                 .opcode = NYR_MACH_COPY,
                                 .dst = vreg(dst),
                                 .src0 = src,
                             });
}

static bool emit_select(nyr_mach_func_t *mach, nyr_mach_reg_t dst,
                        nyr_mach_reg_t condition, nyr_mach_reg_t yes,
                        nyr_mach_reg_t no) {
  return nyr_mach_emit(mach, (nyr_mach_inst_t){
                                 .opcode = NYR_MACH_SELECT,
                                 .dst = vreg(dst),
                                 .src0 = vreg(condition),
                                 .src1 = vreg(yes),
                                 .src2 = vreg(no),
                                 .condition = NYR_MACH_COND_NE,
                             });
}

static bool emit_ret(nyr_mach_func_t *mach, nyr_mach_reg_t value) {
  return nyr_mach_emit(mach, (nyr_mach_inst_t){
                                 .opcode = NYR_MACH_RET,
                                 .src0 = vreg(value),
                                 .effects = NYR_MACH_EFFECT_CONTROL,
                             });
}

static bool selected_result_reuses_dead_arm(void) {
  nyr_mach_func_t mach = {0};
  nyr_mach_regalloc_t alloc = {0};
  nyr_mach_reg_t condition, yes, no, selected;
  uint32_t block = 0;
  char err[256] = {0};
  bool ok = nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &condition) &&
            nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &yes) &&
            nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &no) &&
            nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &selected) &&
            nyr_mach_begin_block(&mach, 0, &block) && block == 0 &&
            emit_copy(&mach, condition, imm(1)) &&
            emit_copy(&mach, yes, imm(2)) && emit_copy(&mach, no, imm(3)) &&
            emit_select(&mach, selected, condition, yes, no) &&
            emit_ret(&mach, selected) &&
            nyr_mach_verify(&mach, 0, err, sizeof(err)) &&
            nyr_mach_regalloc_build_callee_saved_gpr(&mach, 3, &alloc);

  size_t select_pc = ok ? mach.blocks[block].first_inst + 3 : 0;
  const nyr_mach_live_segment_t *yes_seg =
      ok ? nyr_mach_regalloc_segment_at(&alloc, yes, select_pc) : NULL;
  const nyr_mach_live_segment_t *no_seg =
      ok ? nyr_mach_regalloc_segment_at(&alloc, no, select_pc) : NULL;
  const nyr_mach_live_segment_t *selected_seg =
      ok ? nyr_mach_regalloc_segment_at(&alloc, selected, select_pc) : NULL;
  ok = ok && yes_seg && no_seg && selected_seg && yes_seg->end == select_pc &&
       no_seg->end == select_pc && yes_seg->color >= 0 && no_seg->color >= 0 &&
       selected_seg->color >= 0 &&
       (selected_seg->color == yes_seg->color ||
        selected_seg->color == no_seg->color);
  if (!ok)
    fprintf(stderr,
            "[regalloc-select-coalesce] SELECT result did not reuse a dead "
            "arm: yes=%d no=%d result=%d error=%s\n",
            yes_seg ? yes_seg->color : -2, no_seg ? no_seg->color : -2,
            selected_seg ? selected_seg->color : -2,
            err[0] ? err : "no dead-arm color reuse");

  nyr_mach_regalloc_free(&alloc);
  nyr_mach_func_free(&mach);
  return ok;
}

static bool selected_result_does_not_clobber_live_arm(void) {
  nyr_mach_func_t mach = {0};
  nyr_mach_regalloc_t alloc = {0};
  nyr_mach_reg_t condition, yes, no, selected, sum;
  uint32_t block = 0;
  char err[256] = {0};
  bool ok = nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &condition) &&
            nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &yes) &&
            nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &no) &&
            nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &selected) &&
            nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &sum) &&
            nyr_mach_begin_block(&mach, 0, &block) && block == 0 &&
            emit_copy(&mach, condition, imm(1)) &&
            emit_copy(&mach, yes, imm(2)) && emit_copy(&mach, no, imm(3)) &&
            emit_select(&mach, selected, condition, yes, no) &&
            nyr_mach_emit(&mach,
                          (nyr_mach_inst_t){
                              .opcode = NYR_MACH_ADD,
                              .dst = vreg(sum),
                              .src0 = vreg(yes),
                              .src1 = vreg(selected),
                          }) &&
            emit_ret(&mach, sum) &&
            nyr_mach_verify(&mach, 0, err, sizeof(err)) &&
            nyr_mach_regalloc_build_callee_saved_gpr(&mach, 3, &alloc);

  size_t select_pc = ok ? mach.blocks[block].first_inst + 3 : 0;
  const nyr_mach_live_segment_t *yes_seg =
      ok ? nyr_mach_regalloc_segment_at(&alloc, yes, select_pc) : NULL;
  const nyr_mach_live_segment_t *selected_seg =
      ok ? nyr_mach_regalloc_segment_at(&alloc, selected, select_pc) : NULL;
  ok = ok && yes_seg && selected_seg && yes_seg->end > select_pc &&
       (selected_seg->color < 0 || selected_seg->color != yes_seg->color);
  if (!ok)
    fprintf(stderr,
            "[regalloc-select-coalesce] SELECT result reused a still-live "
            "arm: yes=%d/%zu result=%d error=%s\n",
            yes_seg ? yes_seg->color : -2, yes_seg ? yes_seg->end : 0,
            selected_seg ? selected_seg->color : -2,
            err[0] ? err : "live-arm color alias");

  nyr_mach_regalloc_free(&alloc);
  nyr_mach_func_free(&mach);
  return ok;
}

int main(void) {
  bool ok = selected_result_reuses_dead_arm() &&
            selected_result_does_not_clobber_live_arm();
  if (ok)
    puts("[regalloc-select-coalesce] dead-arm reuse and live-arm preservation");
  return ok ? 0 : 1;
}
