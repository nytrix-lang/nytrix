#include "code/backend/internal.h"
#include "code/backend/ir/machine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static nyr_mach_operand_t vreg(nyr_mach_reg_t reg) {
  return (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_VREG,
                              .reg_class = NYR_MACH_REGCLASS_GPR,
                              .as.reg = reg};
}

static size_t occurrences(const char *text, const char *needle) {
  size_t count = 0, len = strlen(needle);
  while ((text = strstr(text, needle)) != NULL) {
    ++count;
    text += len;
  }
  return count;
}

static bool emit_repeated_compare(bool clobber_flags, bool extra_predecessor,
                                  nyr_writer_t *assembly, char *err,
                                  size_t err_size) {
  nyr_mach_func_t mach = {0};
  nyr_mach_reg_t r0, r1, r2, r3, r4;
  uint32_t block;
  bool built =
      nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &r0) &&
      nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &r1) &&
      nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &r2) &&
      nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &r3) &&
      nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &r4) &&
      nyr_mach_begin_block(&mach, 0, &block) &&
      nyr_mach_emit(&mach,
                    (nyr_mach_inst_t){
                        .opcode = NYR_MACH_COPY,
                        .dst = vreg(r0),
                        .src0 = {.kind = NYR_MACH_OPERAND_IMM, .as.imm = 9}}) &&
      nyr_mach_emit(&mach,
                    (nyr_mach_inst_t){.opcode = NYR_MACH_COPY,
                                      .dst = vreg(r2),
                                      .src0 = {.kind = NYR_MACH_OPERAND_IMM,
                                               .as.imm = 12}}) &&
      nyr_mach_emit(&mach, (nyr_mach_inst_t){.opcode = NYR_MACH_CMP,
                                             .dst = vreg(r1),
                                             .src0 = vreg(r0),
                                             .src1 = vreg(r2),
                                             .condition = NYR_MACH_COND_LT}) &&
      nyr_mach_emit(&mach,
                    (nyr_mach_inst_t){.opcode = NYR_MACH_BR_IF,
                                      .src0 = vreg(r1),
                                      .src1 = {.kind = NYR_MACH_OPERAND_BLOCK,
                                               .as.block_index = 3},
                                      .effects = NYR_MACH_EFFECT_CONTROL}) &&
      nyr_mach_begin_block(&mach, 1, &block) &&
      nyr_mach_emit(&mach,
                    (nyr_mach_inst_t){.opcode = NYR_MACH_BR,
                                      .src1 = {.kind = NYR_MACH_OPERAND_BLOCK,
                                               .as.block_index = 2},
                                      .effects = NYR_MACH_EFFECT_CONTROL}) &&
      nyr_mach_begin_block(&mach, 2, &block);
  if (built && clobber_flags)
    built = nyr_mach_emit(&mach, (nyr_mach_inst_t){.opcode = NYR_MACH_ADD,
                                                   .dst = vreg(r3),
                                                   .src0 = vreg(r0),
                                                   .src1 = vreg(r2)});
  else if (built)
    built = nyr_mach_emit(
        &mach,
        (nyr_mach_inst_t){.opcode = NYR_MACH_COPY,
                          .dst = vreg(r3),
                          .src0 = {.kind = NYR_MACH_OPERAND_IMM, .as.imm = 1}});
  built =
      built &&
      nyr_mach_emit(&mach, (nyr_mach_inst_t){.opcode = NYR_MACH_CMP,
                                             .dst = vreg(r4),
                                             .src0 = vreg(r0),
                                             .src1 = vreg(r2),
                                             .condition = NYR_MACH_COND_GT}) &&
      nyr_mach_emit(&mach,
                    (nyr_mach_inst_t){.opcode = NYR_MACH_BR_IF,
                                      .src0 = vreg(r4),
                                      .src1 = {.kind = NYR_MACH_OPERAND_BLOCK,
                                               .as.block_index = 3},
                                      .effects = NYR_MACH_EFFECT_CONTROL}) &&
      nyr_mach_emit(&mach,
                    (nyr_mach_inst_t){.opcode = NYR_MACH_BR,
                                      .src1 = {.kind = NYR_MACH_OPERAND_BLOCK,
                                               .as.block_index = 4},
                                      .effects = NYR_MACH_EFFECT_CONTROL}) &&
      nyr_mach_begin_block(&mach, 3, &block) &&
      nyr_mach_emit(&mach,
                    (nyr_mach_inst_t){.opcode = NYR_MACH_RET,
                                      .src0 = vreg(r0),
                                      .effects = NYR_MACH_EFFECT_CONTROL}) &&
      nyr_mach_begin_block(&mach, 4, &block);
  if (built && extra_predecessor)
    built = nyr_mach_emit(
        &mach, (nyr_mach_inst_t){.opcode = NYR_MACH_BR,
                                 .src1 = {.kind = NYR_MACH_OPERAND_BLOCK,
                                          .as.block_index = 2},
                                 .effects = NYR_MACH_EFFECT_CONTROL});
  else if (built)
    built = nyr_mach_emit(
        &mach, (nyr_mach_inst_t){.opcode = NYR_MACH_RET,
                                 .src0 = vreg(r0),
                                 .effects = NYR_MACH_EFFECT_CONTROL});
  bool ok = built && nyr_mach_verify(&mach, 0, err, err_size) &&
            nyr_x86_64_emit_mach_scalar(
                assembly,
                &(ny_native_target_info_t){.target = NY_NATIVE_TARGET_X86_64,
                                           .abi = NY_NATIVE_ABI_SYSV,
                                           .object_format = "elf",
                                           .symbol_prefix = ""},
                &mach, "cmp_chain", false, err, err_size);
  nyr_mach_func_free(&mach);
  return ok;
}

int main(void) {
  nyr_mach_func_t mach = {0};
  nyr_mach_reg_t r0, r1, r2;
  uint32_t b0, b1, b2;
  bool built =
      nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &r0) &&
      nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &r1) &&
      nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &r2) &&
      nyr_mach_begin_block(&mach, 0, &b0) &&
      nyr_mach_emit(&mach,
                    (nyr_mach_inst_t){
                        .opcode = NYR_MACH_COPY,
                        .dst = vreg(r0),
                        .src0 = {.kind = NYR_MACH_OPERAND_IMM, .as.imm = 9}}) &&
      nyr_mach_emit(&mach,
                    (nyr_mach_inst_t){.opcode = NYR_MACH_COPY,
                                      .dst = vreg(r2),
                                      .src0 = {.kind = NYR_MACH_OPERAND_IMM,
                                               .as.imm = 12}}) &&
      nyr_mach_emit(&mach, (nyr_mach_inst_t){.opcode = NYR_MACH_CMP,
                                             .dst = vreg(r1),
                                             .src0 = vreg(r0),
                                             .src1 = vreg(r2),
                                             .condition = NYR_MACH_COND_LT}) &&
      nyr_mach_emit(&mach,
                    (nyr_mach_inst_t){.opcode = NYR_MACH_BR_IF,
                                      .src0 = vreg(r1),
                                      .src1 = {.kind = NYR_MACH_OPERAND_BLOCK,
                                               .as.block_index = 1},
                                      .effects = NYR_MACH_EFFECT_CONTROL}) &&
      nyr_mach_emit(&mach,
                    (nyr_mach_inst_t){.opcode = NYR_MACH_BR,
                                      .src1 = {.kind = NYR_MACH_OPERAND_BLOCK,
                                               .as.block_index = 2},
                                      .effects = NYR_MACH_EFFECT_CONTROL}) &&
      nyr_mach_begin_block(&mach, 1, &b1) &&
      nyr_mach_emit(&mach,
                    (nyr_mach_inst_t){.opcode = NYR_MACH_RET,
                                      .src0 = vreg(r0),
                                      .effects = NYR_MACH_EFFECT_CONTROL}) &&
      nyr_mach_begin_block(&mach, 2, &b2) &&
      nyr_mach_emit(&mach,
                    (nyr_mach_inst_t){.opcode = NYR_MACH_RET,
                                      .src0 = vreg(r0),
                                      .effects = NYR_MACH_EFFECT_CONTROL});
  (void)b0;
  (void)b1;
  (void)b2;
  char err[256] = {0};
  if (!built || !nyr_mach_verify(&mach, 0, err, sizeof(err))) {
    fprintf(stderr, "[machine-cmp-branch] invalid machine IR: %s\n", err);
    nyr_mach_func_free(&mach);
    return 1;
  }

  static const ny_native_target_info_t target = {
      .target = NY_NATIVE_TARGET_X86_64,
      .abi = NY_NATIVE_ABI_SYSV,
      .object_format = "elf",
      .symbol_prefix = "",
  };
  nyr_writer_t assembly = {0};
  bool emitted = nyr_x86_64_emit_mach_scalar(
      &assembly, &target, &mach, "cmp_branch", false, err, sizeof(err));
  nyr_mach_func_free(&mach);
  bool fused =
      emitted && occurrences(assembly.data, "\tcmpq\t$12, %rax\n") == 1 &&
      occurrences(assembly.data, "\tjl\t.Lny_mach_cmp_branch_1\n") == 1 &&
      occurrences(assembly.data, "\tsetl\t%al\n") == 0 &&
      occurrences(assembly.data, "\tcmpq\t$0,") == 0;
  if (!fused) {
    fprintf(stderr, "[machine-cmp-branch] fusion missing: %s\n%s", err,
            assembly.data ? assembly.data : "<no assembly>\n");
    free(assembly.data);
    return 1;
  }
  free(assembly.data);

  nyr_writer_t repeated = {0};
  bool reused =
      emit_repeated_compare(false, false, &repeated, err, sizeof(err)) &&
      occurrences(repeated.data, "\tcmpq\t$12, %rax\n") == 1 &&
      occurrences(repeated.data, "\tjl\t.Lny_mach_cmp_chain_3\n") == 1 &&
      occurrences(repeated.data, "\tjg\t.Lny_mach_cmp_chain_3\n") == 1;
  free(repeated.data);
  nyr_writer_t clobbered = {0};
  bool rejected =
      emit_repeated_compare(true, false, &clobbered, err, sizeof(err)) &&
      occurrences(clobbered.data, "\tcmpq\t$12, %rax\n") == 2;
  free(clobbered.data);
  nyr_writer_t multiple_preds = {0};
  bool rejected_multiple_preds =
      emit_repeated_compare(false, true, &multiple_preds, err, sizeof(err)) &&
      occurrences(multiple_preds.data, "\tcmpq\t$12, %rax\n") == 2;
  if (!reused || !rejected || !rejected_multiple_preds) {
    fprintf(stderr,
            "[machine-cmp-branch] cross-block compare reuse failed: %s\n",
            err[0] ? err : "fusion or rejection assertion");
    free(multiple_preds.data);
    return 1;
  }
  free(multiple_preds.data);
  puts("[machine-cmp-branch] compare branches directly with signed jcc");
  return 0;
}
