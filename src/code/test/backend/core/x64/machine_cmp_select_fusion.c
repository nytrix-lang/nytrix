/*
 * Regression tests for fusing machine compares with conditional selects.
 */
#include "code/backend/core/internal.h"
#include "code/backend/lower/machine.h"
#include "code/backend/lower/native.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static nyr_mach_operand_t vreg(nyr_mach_reg_t v) {
  return (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_VREG,
                              .reg_class = NYR_MACH_REGCLASS_GPR,
                              .as.reg = v};
}

static bool check_select(nyr_mach_cond_t condition, unsigned arms,
                         bool second_use) {
  nyr_mach_func_t m = {0};
  nyr_mach_reg_t a, b, test, result, again;
  uint32_t block;
  char err[256] = {0};
  bool ok = nyr_mach_alloc_typed_vreg(&m, NYR_MACH_TYPE_I64, &a) &&
            nyr_mach_alloc_typed_vreg(&m, NYR_MACH_TYPE_I64, &b) &&
            nyr_mach_alloc_typed_vreg(&m, NYR_MACH_TYPE_I64, &test) &&
            nyr_mach_alloc_typed_vreg(&m, NYR_MACH_TYPE_I64, &result) &&
            nyr_mach_alloc_typed_vreg(&m, NYR_MACH_TYPE_I64, &again) &&
            nyr_mach_begin_block(&m, 0, &block);
  ok =
      ok &&
      nyr_mach_emit(&m, (nyr_mach_inst_t){.opcode = NYR_MACH_COPY,
                                          .dst = vreg(a),
                                          .src0 = {.kind = NYR_MACH_OPERAND_IMM,
                                                   .as.imm = -7}}) &&
      nyr_mach_emit(&m, (nyr_mach_inst_t){.opcode = NYR_MACH_COPY,
                                          .dst = vreg(b),
                                          .src0 = {.kind = NYR_MACH_OPERAND_IMM,
                                                   .as.imm = 13}}) &&
      nyr_mach_emit(&m, (nyr_mach_inst_t){.opcode = NYR_MACH_CMP,
                                          .dst = vreg(test),
                                          .src0 = vreg(a),
                                          .src1 = vreg(b),
                                          .condition = condition}) &&
      nyr_mach_emit(&m, (nyr_mach_inst_t){.opcode = NYR_MACH_SELECT,
                                          .dst = vreg(result),
                                          .src0 = vreg(test),
                                          .src1 = vreg(arms == 1 ? b : a),
                                          .src2 = vreg(arms == 2 ? a : b),
                                          .condition = NYR_MACH_COND_NE});
  if (second_use)
    ok = ok && nyr_mach_emit(&m, (nyr_mach_inst_t){.opcode = NYR_MACH_COPY,
                                                   .dst = vreg(again),
                                                   .src0 = vreg(test)});
  ok = ok &&
       nyr_mach_emit(&m, (nyr_mach_inst_t){.opcode = NYR_MACH_RET,
                                           .src0 = vreg(result),
                                           .effects = NYR_MACH_EFFECT_CONTROL});
  nyr_writer_t w = {0};
  const ny_native_target_info_t target = {.target = NY_NATIVE_TARGET_X86_64,
                                          .abi = NY_NATIVE_ABI_SYSV,
                                          .object_format = "elf",
                                          .symbol_prefix = ""};
  ok = ok && nyr_mach_verify(&m, 0, err, sizeof(err)) &&
       nyr_x86_64_emit_mach_scalar(&w, &target, &m, "cmp_select", false, false,
                                   err, sizeof(err));
  if (ok) {
    bool materialized =
        strstr(w.data, "\tset") != NULL && strstr(w.data, "\ttestq") != NULL;
    ok = materialized == second_use && strstr(w.data, "\tcmov") != NULL;
  }
  if (!ok)
    fprintf(stderr, "[machine-cmp-select] cond=%d arms=%u extra=%d %s\n%s",
            condition, arms, second_use, err, w.data ? w.data : "<no asm>\n");
  free(w.data);
  nyr_mach_func_free(&m);
  return ok;
}

int main(void) {
  const nyr_mach_cond_t conditions[] = {NYR_MACH_COND_EQ, NYR_MACH_COND_NE,
                                        NYR_MACH_COND_LT, NYR_MACH_COND_LE,
                                        NYR_MACH_COND_GT, NYR_MACH_COND_GE};
  bool ok = true;
  for (size_t c = 0; c < sizeof(conditions) / sizeof(conditions[0]); ++c)
    for (unsigned arms = 0; arms < 3; ++arms)
      for (unsigned extra = 0; extra < 2; ++extra)
        ok = check_select(conditions[c], arms, extra) && ok;
  if (ok)
    puts("[machine-cmp-select] 36 match/nonmatch cases passed");
  return ok ? 0 : 1;
}
