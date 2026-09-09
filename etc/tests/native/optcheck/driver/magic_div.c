/*
 * Machine object encoding regression: a constant crosses a block boundary and
 * two COPY moves before DIV/MOD consume it.  The synthetic dynamic divisor
 * must retain the idivq fallback.
 */
#include "code/native/object/internal.h"

#include <stdio.h>
#include <string.h>

static ny_mach_operand_t vreg(ny_mach_reg_t reg) {
  return (ny_mach_operand_t){.kind = NY_MACH_OPERAND_VREG,
                             .reg_class = NY_MACH_REGCLASS_GPR,
                             .as.reg = reg};
}

static ny_mach_operand_t imm(int64_t value) {
  return (ny_mach_operand_t){.kind = NY_MACH_OPERAND_IMM, .as.imm = value};
}

static bool emit(ny_mach_func_t *mach, ny_mach_opcode_t opcode,
                 ny_mach_operand_t dst, ny_mach_operand_t src0,
                 ny_mach_operand_t src1, unsigned effects) {
  return ny_mach_emit(mach, (ny_mach_inst_t){.opcode = opcode,
                                              .dst = dst,
                                              .src0 = src0,
                                              .src1 = src1,
                                              .effects = effects});
}

int main(void) {
  ny_mach_func_t mach = {0};
  ny_mach_reg_t dividend, divisor, copy0, copy1, quotient, remainder;
  ny_mach_reg_t dynamic_divisor, dynamic_quotient;
  uint32_t block = 0;
  char err[256] = {0};
  if (!ny_mach_begin_block(&mach, 0, &block) || block != 0 ||
      !ny_mach_alloc_typed_vreg(&mach, NY_MACH_TYPE_I64, &dividend) ||
      !ny_mach_alloc_typed_vreg(&mach, NY_MACH_TYPE_I64, &divisor) ||
      !ny_mach_alloc_typed_vreg(&mach, NY_MACH_TYPE_I64, &copy0) ||
      !ny_mach_alloc_typed_vreg(&mach, NY_MACH_TYPE_I64, &copy1) ||
      !ny_mach_alloc_typed_vreg(&mach, NY_MACH_TYPE_I64, &quotient) ||
      !ny_mach_alloc_typed_vreg(&mach, NY_MACH_TYPE_I64, &remainder) ||
      !ny_mach_alloc_typed_vreg(&mach, NY_MACH_TYPE_I64, &dynamic_divisor) ||
      !ny_mach_alloc_typed_vreg(&mach, NY_MACH_TYPE_I64, &dynamic_quotient) ||
      !emit(&mach, NY_MACH_COPY, vreg(dividend), imm(123),
            (ny_mach_operand_t){0}, NY_MACH_EFFECT_NONE) ||
      !emit(&mach, NY_MACH_COPY, vreg(divisor), imm(97),
            (ny_mach_operand_t){0}, NY_MACH_EFFECT_NONE) ||
      !emit(&mach, NY_MACH_BR, (ny_mach_operand_t){0},
            (ny_mach_operand_t){0},
            (ny_mach_operand_t){.kind = NY_MACH_OPERAND_BLOCK,
                                .as.block_index = 1},
            NY_MACH_EFFECT_CONTROL) ||
      !ny_mach_begin_block(&mach, 1, &block) || block != 1 ||
      !emit(&mach, NY_MACH_COPY, vreg(copy0), vreg(divisor),
            (ny_mach_operand_t){0}, NY_MACH_EFFECT_NONE) ||
      !emit(&mach, NY_MACH_COPY, vreg(copy1), vreg(copy0),
            (ny_mach_operand_t){0}, NY_MACH_EFFECT_NONE) ||
      !emit(&mach, NY_MACH_DIV, vreg(quotient), vreg(dividend), vreg(copy1),
            NY_MACH_EFFECT_NONE) ||
      !emit(&mach, NY_MACH_MOD, vreg(remainder), vreg(dividend), vreg(copy1),
            NY_MACH_EFFECT_NONE) ||
      !emit(&mach, NY_MACH_DIV, vreg(dynamic_quotient), vreg(dividend),
            vreg(dynamic_divisor), NY_MACH_EFFECT_NONE) ||
      !emit(&mach, NY_MACH_RET, (ny_mach_operand_t){0}, vreg(quotient),
            (ny_mach_operand_t){0}, NY_MACH_EFFECT_CONTROL) ||
      !ny_mach_verify(&mach, 0, err, sizeof(err))) {
    fprintf(stderr, "[magic-div] setup failed: %s\n", err);
    ny_mach_func_free(&mach);
    return 1;
  }

  unsigned long long before_magic = 0, before_idiv = 0;
  unsigned long long after_magic = 0, after_idiv = 0;
  ny_native_mach_div_stats(&before_magic, &before_idiv);
  ny_obj_buf_t code = {0};
  ny_x64_obj_symbol_def_t defs[1] = {0};
  ny_x64_obj_reloc_t relocs[NY_X64_OBJ_MAX_RELOCS] = {0};
  size_t def_count = 0, reloc_count = 0;
  ny_native_target_info_t target = {
      .target = NY_NATIVE_TARGET_X86_64,
      .abi = NY_NATIVE_ABI_SYSV,
      .symbol_prefix = "",
  };
  bool encoded = ny_x64_mach_append_function(
      &code, defs, &def_count, relocs, &reloc_count, &mach, &target,
      "magic_div", false, err, sizeof(err));
  ny_obj_free(&code);
  ny_mach_func_free(&mach);
  if (!encoded) {
    fprintf(stderr, "[magic-div] encode failed: %s\n", err);
    return 1;
  }
  ny_native_mach_div_stats(&after_magic, &after_idiv);
  unsigned long long magic = after_magic - before_magic;
  unsigned long long idiv = after_idiv - before_idiv;
  if (magic != 2 || idiv != 1) {
    fprintf(stderr, "[magic-div] want magic=2 idiv=1 got magic=%llu idiv=%llu\n",
            magic, idiv);
    return 1;
  }
  printf("[magic-div] magic=2 idiv=1\n");
  return 0;
}
