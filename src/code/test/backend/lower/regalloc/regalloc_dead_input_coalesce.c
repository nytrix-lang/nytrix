/*
 * Dying arithmetic inputs can share the result register, live inputs cannot.
 */
#include "code/backend/lower/machine.h"
#include "code/backend/mach/object.h"
#include "opt.h"

#include <stdint.h>
#include <stdio.h>

static nyr_mach_operand_t vreg(nyr_mach_reg_t reg) {
  return (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_VREG,
                              .reg_class = NYR_MACH_REGCLASS_GPR,
                              .as.reg = reg};
}

static nyr_mach_operand_t typed_vreg(nyr_mach_reg_t reg,
                                     nyr_mach_reg_class_t reg_class) {
  return (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_VREG,
                              .reg_class = reg_class,
                              .as.reg = reg};
}

static nyr_mach_operand_t imm(int64_t value) {
  return (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_IMM,
                              .as.imm = value};
}

static bool emit(nyr_mach_func_t *mach, nyr_mach_opcode_t opcode,
                 nyr_mach_operand_t dst, nyr_mach_operand_t src0,
                 nyr_mach_operand_t src1) {
  return nyr_mach_emit(mach, (nyr_mach_inst_t){
                                  .opcode = opcode,
                                  .dst = dst,
                                  .src0 = src0,
                                  .src1 = src1,
                                  .effects = opcode == NYR_MACH_RET
                                                 ? NYR_MACH_EFFECT_CONTROL
                                                 : NYR_MACH_EFFECT_NONE,
                              });
}

static bool check_case(bool keep_left_live) {
  nyr_mach_func_t mach = {0};
  nyr_mach_regalloc_t alloc = {0};
  nyr_mach_reg_t left, right, sum, extra, result;
  uint32_t block = 0;
  char err[256] = {0};
  bool ok = nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &left) &&
            nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &right) &&
            nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &sum) &&
            (!keep_left_live ||
             (nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &extra) &&
              nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &result))) &&
            nyr_mach_begin_block(&mach, 0, &block) && block == 0 &&
            emit(&mach, NYR_MACH_COPY, vreg(left), imm(7),
                 (nyr_mach_operand_t){0}) &&
            emit(&mach, NYR_MACH_COPY, vreg(right), imm(11),
                 (nyr_mach_operand_t){0}) &&
            emit(&mach, NYR_MACH_ADD, vreg(sum), vreg(left), vreg(right)) &&
            (!keep_left_live ||
             (emit(&mach, NYR_MACH_COPY, vreg(extra), vreg(left),
                   (nyr_mach_operand_t){0}) &&
              emit(&mach, NYR_MACH_ADD, vreg(result), vreg(sum), vreg(extra)) &&
              emit(&mach, NYR_MACH_RET, (nyr_mach_operand_t){0}, vreg(result),
                   (nyr_mach_operand_t){0}))) &&
            (keep_left_live ||
             emit(&mach, NYR_MACH_RET, (nyr_mach_operand_t){0}, vreg(sum),
                  (nyr_mach_operand_t){0})) &&
            nyr_mach_verify(&mach, 0, err, sizeof(err)) &&
            nyr_mach_regalloc_build_callee_saved_gpr(&mach, 3, &alloc);

  size_t add_pc = ok ? mach.blocks[block].first_inst + 2 : 0;
  const nyr_mach_live_segment_t *left_seg =
      ok ? nyr_mach_regalloc_segment_at(&alloc, left, add_pc) : NULL;
  const nyr_mach_live_segment_t *sum_seg =
      ok ? nyr_mach_regalloc_segment_at(&alloc, sum, add_pc) : NULL;
  if (keep_left_live)
    ok = ok && left_seg && sum_seg && left_seg->end > add_pc &&
         (sum_seg->color < 0 || sum_seg->color != left_seg->color);
  else
    ok = ok && left_seg && sum_seg && left_seg->end == add_pc &&
         left_seg->color >= 0 && sum_seg->color == left_seg->color;
  if (!ok)
    fprintf(stderr,
            "[regalloc-dead-input-coalesce] live=%d left=%d/%zu sum=%d "
            "error=%s\n",
            keep_left_live, left_seg ? left_seg->color : -2,
            left_seg ? left_seg->end : 0, sum_seg ? sum_seg->color : -2,
            err[0] ? err : "tied input allocation mismatch");
  nyr_mach_regalloc_free(&alloc);
  nyr_mach_func_free(&mach);
  return ok;
}

static bool check_live_rhs(nyr_mach_reg_class_t reg_class,
                           nyr_mach_type_t type) {
  nyr_mach_func_t mach = {0};
  nyr_mach_regalloc_t alloc = {0};
  nyr_mach_reg_t lhs, rhs, product, consume;
  uint32_t block = 0;
  char err[256] = {0};
  int64_t lhs_bits = type == NYR_MACH_TYPE_F64 ? INT64_C(0x4024000000000000) : 7;
  int64_t rhs_bits = type == NYR_MACH_TYPE_F64 ? INT64_C(0x4024000000000000) : 11;
  bool ok = nyr_mach_alloc_typed_vreg(&mach, type, &lhs) &&
            nyr_mach_alloc_typed_vreg(&mach, type, &rhs) &&
            nyr_mach_alloc_typed_vreg(&mach, type, &product) &&
            nyr_mach_alloc_typed_vreg(&mach, type, &consume) &&
            nyr_mach_begin_block(&mach, 0, &block) && block == 0 &&
            emit(&mach, NYR_MACH_COPY, typed_vreg(lhs, reg_class), imm(lhs_bits),
                 (nyr_mach_operand_t){0}) &&
            emit(&mach, NYR_MACH_COPY, typed_vreg(rhs, reg_class), imm(rhs_bits),
                 (nyr_mach_operand_t){0}) &&
            emit(&mach, NYR_MACH_MUL, typed_vreg(product, reg_class),
                 typed_vreg(lhs, reg_class), typed_vreg(rhs, reg_class)) &&
            emit(&mach, NYR_MACH_MUL, typed_vreg(consume, reg_class),
                 typed_vreg(product, reg_class), typed_vreg(rhs, reg_class)) &&
            emit(&mach, NYR_MACH_RET, (nyr_mach_operand_t){0},
                 typed_vreg(consume, reg_class),
                 (nyr_mach_operand_t){0}) &&
            nyr_mach_verify(&mach, 0, err, sizeof(err)) &&
            nyr_mach_regalloc_build_class(&mach, reg_class, 3, &alloc);
  size_t mul_pc = ok ? mach.blocks[block].first_inst + 2 : 0;
  const nyr_mach_live_segment_t *rhs_seg =
      ok ? nyr_mach_regalloc_segment_at(&alloc, rhs, mul_pc) : NULL;
  const nyr_mach_live_segment_t *product_seg =
      ok ? nyr_mach_regalloc_segment_at(&alloc, product, mul_pc) : NULL;
  ok = ok && rhs_seg && product_seg && rhs_seg->end > mul_pc &&
       rhs_seg->color >= 0 && product_seg->color >= 0 &&
       rhs_seg->color != product_seg->color;
  if (!ok)
    fprintf(stderr,
            "[regalloc-dead-input-coalesce] live rhs class=%d rhs=%d/%zu "
            "result=%d error=%s\n",
            (int)reg_class, rhs_seg ? rhs_seg->color : -2,
            rhs_seg ? rhs_seg->end : 0,
            product_seg ? product_seg->color : -2,
            err[0] ? err : "result overlaps a still-live rhs");
  nyr_mach_regalloc_free(&alloc);
  nyr_mach_func_free(&mach);
  return ok;
}

static bool check_x64_dying_rhs_tie(void) {
  nyr_mach_func_t mach = {0};
  nyr_mach_regalloc_t alloc = {0};
  nyr_mach_reg_t lhs, rhs, product, consume;
  uint32_t block = 0;
  char err[256] = {0};
  bool ok = nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_F64, &lhs) &&
            nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_F64, &rhs) &&
            nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_F64, &product) &&
            nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_F64, &consume) &&
            nyr_mach_begin_block(&mach, 0, &block) && block == 0 &&
            emit(&mach, NYR_MACH_COPY,
                 typed_vreg(lhs, NYR_MACH_REGCLASS_FPR),
                 imm(INT64_C(0x4024000000000000)), (nyr_mach_operand_t){0}) &&
            emit(&mach, NYR_MACH_COPY,
                 typed_vreg(rhs, NYR_MACH_REGCLASS_FPR),
                 imm(INT64_C(0x4024000000000000)), (nyr_mach_operand_t){0}) &&
            emit(&mach, NYR_MACH_MUL,
                 typed_vreg(product, NYR_MACH_REGCLASS_FPR),
                 typed_vreg(lhs, NYR_MACH_REGCLASS_FPR),
                 typed_vreg(rhs, NYR_MACH_REGCLASS_FPR)) &&
            emit(&mach, NYR_MACH_ADD,
                 typed_vreg(consume, NYR_MACH_REGCLASS_FPR),
                 typed_vreg(product, NYR_MACH_REGCLASS_FPR),
                 typed_vreg(lhs, NYR_MACH_REGCLASS_FPR)) &&
            emit(&mach, NYR_MACH_RET, (nyr_mach_operand_t){0},
                 typed_vreg(consume, NYR_MACH_REGCLASS_FPR),
                 (nyr_mach_operand_t){0}) &&
            nyr_mach_verify(&mach, 0, err, sizeof(err)) &&
            nyr_mach_regalloc_build_x64_class(
                &mach, NYR_MACH_REGCLASS_FPR, 4, &alloc);
  size_t mul_pc = ok ? mach.blocks[block].first_inst + 2 : 0;
  const nyr_mach_live_segment_t *rhs_seg =
      ok ? nyr_mach_regalloc_segment_at(&alloc, rhs, mul_pc) : NULL;
  const nyr_mach_live_segment_t *product_seg =
      ok ? nyr_mach_regalloc_segment_at(&alloc, product, mul_pc) : NULL;
  ok = ok && rhs_seg && product_seg && rhs_seg->end == mul_pc &&
       product_seg->color >= 0 && rhs_seg->color == product_seg->color &&
       nyr_mach_regalloc_verify(&mach, &alloc, NYR_MACH_REGCLASS_FPR);
  if (!ok)
    fprintf(stderr,
            "[regalloc-dead-input-coalesce] x64 dying FPR rhs did not share "
            "the result color: rhs=%d/%zu result=%d error=%s\n",
            rhs_seg ? rhs_seg->color : -2, rhs_seg ? rhs_seg->end : 0,
            product_seg ? product_seg->color : -2,
            err[0] ? err : "x64 rhs-tie proof failed");
  nyr_mach_regalloc_free(&alloc);
  nyr_mach_func_free(&mach);
  return ok;
}

static bool emitted_add_with_dead_input_executes(void) {
#if !defined(__x86_64__)
  return true;
#else
  const nyr_target_request_t request = nyr_target_request_make(
      NY_NATIVE_BACKEND_X86_64, NY_NATIVE_ABI_SYSV, "x86_64-linux-gnu", false);
  ny_native_target_info_t target = {0};
  nyr_mach_func_t mach = {0};
  nyr_obj_buf_t code = {0};
  nyr_x64_obj_symbol_def_t defs[1] = {0};
  nyr_x64_obj_reloc_t relocs[1] = {0};
  size_t def_count = 0, reloc_count = 0;
  nyr_mach_reg_t left, right, sum;
  uint32_t block = 0;
  char err[256] = {0};
  bool ok = nyr_target_info_init(&target, &request) &&
            nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &left) &&
            nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &right) &&
            nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &sum) &&
            nyr_mach_begin_block(&mach, 0, &block) && block == 0 &&
            emit(&mach, NYR_MACH_COPY, vreg(left), imm(7),
                 (nyr_mach_operand_t){0}) &&
            emit(&mach, NYR_MACH_COPY, vreg(right), imm(11),
                 (nyr_mach_operand_t){0}) &&
            emit(&mach, NYR_MACH_ADD, vreg(sum), vreg(left), vreg(right)) &&
            emit(&mach, NYR_MACH_RET, (nyr_mach_operand_t){0}, vreg(sum),
                 (nyr_mach_operand_t){0}) &&
            nyr_mach_verify(&mach, target.caps, err, sizeof(err)) &&
            nyr_x64_mach_append_function(
                &code, defs, &def_count, relocs, &reloc_count, &mach, &target,
                "dead_input_add", false, err, sizeof(err));
  int symbol = ok ? nyr_x64_obj_def_index(defs, def_count, "dead_input_add")
                  : -1;
  ok = ok && symbol >= 0 && reloc_count == 0;
  void *mapping = MAP_FAILED;
  if (ok) {
    mapping = nyr_opt_execmem_alloc(code.len);
    ok = mapping != MAP_FAILED;
  }
  if (ok) {
    memcpy(mapping, code.data, code.len);
    ok = nyr_opt_execmem_protect(mapping, code.len) == 0;
  }
  if (ok) {
    typedef int64_t (*fn_t)(void);
    fn_t fn = (fn_t)((unsigned char *)mapping + defs[symbol].off);
    ok = fn() == 18;
  }
  if (!ok)
    fprintf(stderr,
            "[regalloc-dead-input-coalesce] emitted ADD returned wrong result: "
            "relocs=%zu error=%s\n",
            reloc_count, err[0] ? err : "codegen or execution mismatch");
  if (mapping != MAP_FAILED)
    nyr_opt_execmem_free(mapping, code.len);
  nyr_obj_free(&code);
  nyr_mach_func_free(&mach);
  return ok;
#endif
}

int main(void) {
  bool ok = check_case(false) && check_case(true) &&
            check_live_rhs(NYR_MACH_REGCLASS_GPR, NYR_MACH_TYPE_I64) &&
            check_live_rhs(NYR_MACH_REGCLASS_FPR, NYR_MACH_TYPE_F64) &&
            check_x64_dying_rhs_tie() &&
            emitted_add_with_dead_input_executes();
  if (ok)
    puts("[regalloc-dead-input-coalesce] dead arithmetic input reuse is safe");
  return ok ? 0 : 1;
}
