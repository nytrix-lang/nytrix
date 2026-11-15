/*
 * The segment verifier must reject a live range that omits a real operand use,
 * and a colored range that stays live across a fixed-register clobber (the
 * x64 variable-shift count in RCX, allocator color 10).
 */
#include "code/backend/lower/machine.h"

#include <stdio.h>

static nyr_mach_operand_t vreg(nyr_mach_reg_t reg) {
  return (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_VREG,
                              .reg_class = NYR_MACH_REGCLASS_GPR,
                              .as.reg = reg};
}

static nyr_mach_operand_t imm(int64_t value) {
  return (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_IMM, .as.imm = value};
}

static bool emit(nyr_mach_func_t *mach, nyr_mach_opcode_t opcode,
                 nyr_mach_operand_t dst, nyr_mach_operand_t src0,
                 nyr_mach_operand_t src1) {
  return nyr_mach_emit(
      mach, (nyr_mach_inst_t){.opcode = opcode,
                              .dst = dst,
                              .src0 = src0,
                              .src1 = src1,
                              .effects = opcode == NYR_MACH_RET
                                             ? NYR_MACH_EFFECT_CONTROL
                                             : NYR_MACH_EFFECT_NONE});
}

/*
 * x = 5; a = 1; c = 2; s = a << c; ret s + x. x is live across the variable
 * shift, so it may take any color except RCX.
 */
static bool check_fixed_clobber(void) {
  nyr_mach_func_t mach = {0};
  nyr_mach_regalloc_t alloc = {0};
  nyr_mach_reg_t x, a, c, shifted, result;
  uint32_t block;
  char err[256] = {0};
  bool ok =
      nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &x) &&
      nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &a) &&
      nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &c) &&
      nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &shifted) &&
      nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &result) &&
      nyr_mach_begin_block(&mach, 0, &block) && block == 0 &&
      emit(&mach, NYR_MACH_COPY, vreg(x), imm(5), (nyr_mach_operand_t){0}) &&
      emit(&mach, NYR_MACH_COPY, vreg(a), imm(1), (nyr_mach_operand_t){0}) &&
      emit(&mach, NYR_MACH_COPY, vreg(c), imm(2), (nyr_mach_operand_t){0}) &&
      emit(&mach, NYR_MACH_SHL, vreg(shifted), vreg(a), vreg(c)) &&
      emit(&mach, NYR_MACH_ADD, vreg(result), vreg(shifted), vreg(x)) &&
      emit(&mach, NYR_MACH_RET, (nyr_mach_operand_t){0}, vreg(result),
           (nyr_mach_operand_t){0}) &&
      nyr_mach_verify(&mach, 0, err, sizeof(err)) &&
      nyr_mach_regalloc_build(&mach, 12, &alloc) &&
      nyr_mach_regalloc_verify(&mach, &alloc, NYR_MACH_REGCLASS_GPR);
  size_t xs = SIZE_MAX;
  for (size_t i = 0; ok && i < alloc.segment_len; ++i)
    if (alloc.segments[i].vreg == x && alloc.segments[i].start <= 3 &&
        alloc.segments[i].end > 3)
      xs = i;
  ok = ok && xs != SIZE_MAX && alloc.segments[xs].color != 10;
  if (ok) {
    nyr_mach_live_segment_t saved = alloc.segments[xs];
    alloc.segments[xs].color = 10;
    bool accepted = nyr_mach_regalloc_verify(&mach, &alloc,
                                             NYR_MACH_REGCLASS_GPR);
    alloc.segments[xs] = saved;
    ok = !accepted &&
         nyr_mach_regalloc_verify(&mach, &alloc, NYR_MACH_REGCLASS_GPR);
  }
  if (!ok)
    fprintf(stderr, "[regalloc-verifier-coverage] RCX range across a "
                    "variable shift was accepted (%s)\n",
            err[0] ? err : "fixed-clobber invariant failed");
  nyr_mach_regalloc_free(&alloc);
  nyr_mach_func_free(&mach);
  return ok;
}

int main(void) {
  nyr_mach_func_t mach = {0};
  nyr_mach_regalloc_t alloc = {0};
  nyr_mach_reg_t lhs, rhs, result;
  uint32_t block;
  char err[256] = {0};
  bool ok =
      nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &lhs) &&
      nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &rhs) &&
      nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &result) &&
      nyr_mach_begin_block(&mach, 0, &block) && block == 0 &&
      emit(&mach, NYR_MACH_COPY, vreg(lhs), imm(10), (nyr_mach_operand_t){0}) &&
      emit(&mach, NYR_MACH_COPY, vreg(rhs), imm(20), (nyr_mach_operand_t){0}) &&
      emit(&mach, NYR_MACH_ADD, vreg(result), vreg(lhs), vreg(rhs)) &&
      emit(&mach, NYR_MACH_RET, (nyr_mach_operand_t){0}, vreg(result),
           (nyr_mach_operand_t){0}) &&
      nyr_mach_verify(&mach, 0, err, sizeof(err)) &&
      nyr_mach_regalloc_build(&mach, 8, &alloc) &&
      nyr_mach_regalloc_verify(&mach, &alloc, NYR_MACH_REGCLASS_GPR);

  size_t lhs_segment = SIZE_MAX;
  for (size_t i = 0; ok && i < alloc.segment_len; ++i)
    if (alloc.segments[i].vreg == lhs && alloc.segments[i].block == block) {
      lhs_segment = i;
      break;
    }
  ok = ok && lhs_segment != SIZE_MAX && alloc.segments[lhs_segment].start == 0 &&
       alloc.segments[lhs_segment].end >= 2;
  if (ok) {
    nyr_mach_live_segment_t saved = alloc.segments[lhs_segment];
    alloc.segments[lhs_segment].end = 1;
    bool accepted_missing_use =
        nyr_mach_regalloc_verify(&mach, &alloc, NYR_MACH_REGCLASS_GPR);
    alloc.segments[lhs_segment] = saved;
    ok = !accepted_missing_use &&
         nyr_mach_regalloc_verify(&mach, &alloc, NYR_MACH_REGCLASS_GPR);
  }
  if (!ok)
    fprintf(stderr,
            "[regalloc-verifier-coverage] rejected valid ranges or accepted a "
            "range missing lhs use at pc=2 (%s)\n",
            err[0] ? err : "coverage invariant failed");
  nyr_mach_regalloc_free(&alloc);
  nyr_mach_func_free(&mach);
  ok = ok && check_fixed_clobber();
  if (ok)
    puts("[regalloc-verifier-coverage] missing use and fixed clobber rejected");
  return ok ? 0 : 1;
}
