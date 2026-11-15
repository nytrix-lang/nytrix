/*
 * Regression tests for 32-bit immediate selection in machine arithmetic.
 */
#include "code/backend/core/internal.h"
#include "code/backend/lower/machine.h"
#include "code/backend/lower/native.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static nyr_mach_operand_t vr(nyr_mach_reg_t r) {
  return (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_VREG,
                              .reg_class = NYR_MACH_REGCLASS_GPR,
                              .as.reg = r};
}

static bool check(unsigned op, int64_t literal, bool narrow, bool lhs) {
  static const nyr_mach_opcode_t opcode[] = {NYR_MACH_ADD, NYR_MACH_SUB,
                                             NYR_MACH_MUL, NYR_MACH_AND,
                                             NYR_MACH_OR,  NYR_MACH_XOR};
  static const char *const mnemonic[] = {"add", "sub", "imul",
                                         "and", "or",  "xor"};
  nyr_mach_func_t m = {0};
  nyr_mach_reg_t input, constant, dst;
  uint32_t block, frame_index;
  char err[256] = {0};
  nyr_mach_operand_t value = {.kind = NYR_MACH_OPERAND_FRAME};
  nyr_mach_operand_t imm = {.kind = NYR_MACH_OPERAND_IMM, .as.imm = literal};
  bool ok = nyr_mach_alloc_typed_vreg(&m, NYR_MACH_TYPE_I64, &input) &&
            nyr_mach_alloc_typed_vreg(&m, NYR_MACH_TYPE_I64, &constant) &&
            nyr_mach_alloc_typed_vreg(&m, NYR_MACH_TYPE_I64, &dst) &&
            nyr_mach_alloc_typed_frame_slot(&m, 8, 8, NYR_MACH_TYPE_I64, false,
                                            &frame_index) &&
            nyr_mach_begin_block(&m, 0, &block) && block == 0;
  nyr_mach_operand_t vinput = vr(input), vconst = vr(constant), vdst = vr(dst);
  value.as.frame_index = frame_index;
  ok = ok &&
       nyr_mach_emit(
           &m, (nyr_mach_inst_t){.opcode = NYR_MACH_LOAD,
                                 .dst = vinput,
                                 .src0 = value,
                                 .effects = NYR_MACH_EFFECT_READ_MEMORY}) &&
       nyr_mach_emit(&m, (nyr_mach_inst_t){.opcode = NYR_MACH_COPY,
                                           .dst = vconst,
                                           .src0 = imm}) &&
       nyr_mach_emit(&m, (nyr_mach_inst_t){.opcode = opcode[op],
                                           .dst = vdst,
                                           .src0 = lhs ? vconst : vinput,
                                           .src1 = lhs ? vinput : vconst,
                                           .narrow32 = narrow}) &&
       nyr_mach_emit(&m, (nyr_mach_inst_t){.opcode = NYR_MACH_RET,
                                           .src0 = vdst,
                                           .effects = NYR_MACH_EFFECT_CONTROL});
  nyr_writer_t w = {0};
  ny_native_target_info_t target = {.target = NY_NATIVE_TARGET_X86_64,
                                    .abi = NY_NATIVE_ABI_SYSV,
                                    .object_format = "elf",
                                    .symbol_prefix = ""};
  ok = ok && nyr_mach_verify(&m, 0, err, sizeof(err)) &&
       nyr_x86_64_emit_mach_scalar(&w, &target, &m, "imm32", false, false, err,
                                   sizeof(err));
  if (ok) {
    char expected[80];
    int64_t imm32 = (int64_t)(int32_t)literal;
    bool compact = narrow || literal == imm32;
    snprintf(expected, sizeof(expected), "\t%sl\t$%" PRId64 ", %%eax",
             mnemonic[op], imm32);
    if (!compact)
      ok = true;
    else if (narrow)
      ok = strstr(w.data, expected) != NULL ||
           (op == 2 && (imm32 == 3 || imm32 == 5 || imm32 == 9) &&
            strstr(w.data, "\tleal\t") != NULL);
    else
      ok = true;
  }
  if (!ok)
    fprintf(stderr,
            "[machine-imm32] op=%u lit=%" PRId64 " narrow=%d lhs=%d: %s\n%s\n",
            op, literal, narrow, lhs, err, w.data ? w.data : "<no asm>");
  free(w.data);
  nyr_mach_func_free(&m);
  return ok;
}

int main(void) {
  static const int64_t constants[] = {INT64_C(0x7fffffff),
                                      INT64_C(0x80000000),
                                      INT64_C(0xffffffff),
                                      INT64_C(0x100000000),
                                      INT64_MIN,
                                      -1,
                                      0,
                                      3,
                                      5,
                                      9};
  bool ok = true;
  unsigned count = 0;
  for (unsigned op = 0; op < 6; ++op)
    for (size_t lit = 0; lit < sizeof(constants) / sizeof(constants[0]); ++lit)
      for (unsigned narrow = 0; narrow < 2; ++narrow)
        for (unsigned lhs = 0; lhs < 2; ++lhs) {
          if (lhs && op == 1)
            continue;
          ok = check(op, constants[lit], narrow != 0, lhs != 0) && ok;
          ++count;
        }
  if (ok)
    printf("[machine-imm32] %u immediate-lowering cases passed\n", count);
  return ok ? 0 : 1;
}
