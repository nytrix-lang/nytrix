/*
 * Compile-only scaling guard for allocator COPY-affinity lookup.
 */
#define _POSIX_C_SOURCE 200809L
#include "code/backend/ir/machine.h"

#include <stdio.h>
#include <time.h>

enum { COPY_PAIRS = 12000 };

static nyr_mach_operand_t vreg(nyr_mach_reg_t value) {
  return (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_VREG,
                             .reg_class = NYR_MACH_REGCLASS_GPR,
                             .as.reg = value};
}

static double elapsed_ms(struct timespec begin, struct timespec end) {
  return (double)(end.tv_sec - begin.tv_sec) * 1000.0 +
         (double)(end.tv_nsec - begin.tv_nsec) / 1000000.0;
}

int main(void) {
  nyr_mach_func_t mach = {0};
  nyr_mach_regalloc_t alloc = {0};
  nyr_mach_reg_t accumulator = NYR_MACH_REG_INVALID;
  uint32_t block = UINT32_MAX;
  char err[160] = {0};
  bool ok =
      nyr_mach_begin_block(&mach, 0, &block) &&
      nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &accumulator) &&
      nyr_mach_emit(&mach, (nyr_mach_inst_t){ .opcode = NYR_MACH_COPY,
                                           .dst = vreg(accumulator),
                                           .src0 = {.kind = NYR_MACH_OPERAND_IMM,
                                                    .as.imm = 0} });
  for (int i = 0; ok && i < COPY_PAIRS; ++i) {
    nyr_mach_reg_t source = NYR_MACH_REG_INVALID;
    nyr_mach_reg_t copy = NYR_MACH_REG_INVALID;
    nyr_mach_reg_t next = NYR_MACH_REG_INVALID;
    ok = nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &source) &&
         nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &copy) &&
         nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &next) &&
         nyr_mach_emit(&mach,
                      (nyr_mach_inst_t){ .opcode = NYR_MACH_COPY,
                                       .dst = vreg(source),
                                       .src0 = {.kind = NYR_MACH_OPERAND_IMM,
                                                .as.imm = i} }) &&
         nyr_mach_emit(&mach, (nyr_mach_inst_t){ .opcode = NYR_MACH_COPY,
                                              .dst = vreg(copy),
                                              .src0 = vreg(source) }) &&
         nyr_mach_emit(&mach, (nyr_mach_inst_t){ .opcode = NYR_MACH_ADD,
                                              .dst = vreg(next),
                                              .src0 = vreg(accumulator),
                                              .src1 = vreg(copy) });
    accumulator = next;
  }
  ok = ok &&
       nyr_mach_emit(&mach,
                    (nyr_mach_inst_t){ .opcode = NYR_MACH_RET,
                                     .src0 = vreg(accumulator),
                                     .effects = NYR_MACH_EFFECT_CONTROL }) &&
       nyr_mach_verify(&mach, 0, err, sizeof(err));

  double samples_ms[3] = {0};
  size_t segment_count = 0;
  for (size_t sample = 0; ok && sample < 3; ++sample) {
    struct timespec begin = {0}, end = {0};
    clock_gettime(CLOCK_MONOTONIC, &begin);
    ok = nyr_mach_regalloc_build(&mach, 5, &alloc);
    clock_gettime(CLOCK_MONOTONIC, &end);
    samples_ms[sample] = elapsed_ms(begin, end);
    segment_count = alloc.segment_len;
    nyr_mach_regalloc_free(&alloc);
  }
  if (samples_ms[0] > samples_ms[1]) {
    double swap = samples_ms[0];
    samples_ms[0] = samples_ms[1];
    samples_ms[1] = swap;
  }
  if (samples_ms[1] > samples_ms[2]) {
    double swap = samples_ms[1];
    samples_ms[1] = samples_ms[2];
    samples_ms[2] = swap;
  }
  if (samples_ms[0] > samples_ms[1]) {
    double swap = samples_ms[0];
    samples_ms[0] = samples_ms[1];
    samples_ms[1] = swap;
  }
  if (!ok)
    fprintf(stderr, "[machine-copy-scaling] %s\n",
            err[0] ? err : "allocator rejected copy-heavy function");
  else
    printf("[machine-copy-scaling] pairs=%d insts=%zu vregs=%zu "
           "segments=%zu regalloc-median=%.2fms runs=3\n",
           COPY_PAIRS, mach.inst_len, mach.vreg_len, segment_count,
           samples_ms[1]);

  nyr_mach_regalloc_free(&alloc);
  nyr_mach_func_free(&mach);
  return ok ? 0 : 1;
}
