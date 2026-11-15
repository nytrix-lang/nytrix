/*
 * Regression test for post-register-allocation machine scheduling barriers.
 */
#include "code/backend/lower/machine.h"

#include <stdio.h>
#include <string.h>

static nyr_mach_operand_t vreg(nyr_mach_reg_t reg,
                                nyr_mach_reg_class_t cls) {
  return (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_VREG,
                              .reg_class = cls, .as.reg = reg};
}

static bool vector_load_users(void) {
  /*
   * Independent IV update, RAW/WAR/WAW hazards, effect and fixed-register
   * barriers, followed by an already-adjacent RHS consumer.
   */
  for (unsigned test = 0; test < 7; ++test) {
    nyr_mach_func_t m = {0};
    nyr_mach_reg_t r[7];
    bool ok = true;
    uint32_t block;
    for (size_t i = 0; ok && i < 7; ++i) {
      nyr_mach_reg_class_t cls = i < 3 ? NYR_MACH_REGCLASS_GPR
                                      : NYR_MACH_REGCLASS_VECTOR;
      ok = nyr_mach_alloc_vreg(&m, cls, &r[i]);
      if (ok)
        m.vreg_types[r[i]] = i < 3 ? NYR_MACH_TYPE_I64
                                  : NYR_MACH_TYPE_V128_I64;
    }
    if (!ok) { nyr_mach_func_free(&m); return false; }
    nyr_mach_inst_t load = {.opcode = NYR_MACH_LOAD,
        .dst = vreg(r[3], NYR_MACH_REGCLASS_VECTOR),
        .src0 = vreg(r[0], NYR_MACH_REGCLASS_GPR),
        .effects = NYR_MACH_EFFECT_READ_MEMORY};
    nyr_mach_inst_t filler = {.opcode = NYR_MACH_ADD,
        .dst = vreg(r[1], NYR_MACH_REGCLASS_GPR),
        .src0 = vreg(r[0], NYR_MACH_REGCLASS_GPR),
        .src1 = vreg(r[2], NYR_MACH_REGCLASS_GPR)};
    nyr_mach_inst_t use = {.opcode = NYR_MACH_ADD,
        .dst = vreg(r[5], NYR_MACH_REGCLASS_VECTOR),
        .src0 = vreg(r[3], NYR_MACH_REGCLASS_VECTOR),
        .src1 = vreg(r[4], NYR_MACH_REGCLASS_VECTOR)};
    if (test == 1) filler.dst = use.src1; /* RAW */
    if (test == 2) filler.src0 = use.dst; /* WAR */
    if (test == 3) filler.dst = use.dst; /* WAW */
    if (test == 4) filler.effects = NYR_MACH_EFFECT_READ_MEMORY;
    if (test == 5) filler.src0.kind = NYR_MACH_OPERAND_PREG;
    if (test == 6) {
      nyr_mach_operand_t tmp = use.src0;
      use.src0 = use.src1;
      use.src1 = tmp;
    }
    ok = nyr_mach_begin_block(&m, 0, &block) &&
         nyr_mach_emit(&m, load) &&
         (test == 6 || nyr_mach_emit(&m, filler)) &&
         nyr_mach_emit(&m, use) && nyr_mach_schedule_post_ra(&m);
    if (ok && (test == 0 || test == 6))
      ok = m.insts[1].dst.as.reg == r[5] &&
           m.insts[1].src1.as.reg == r[3];
    else if (ok)
      ok = memcmp(&m.insts[1], &filler, sizeof(filler)) == 0 &&
           memcmp(&m.insts[2], &use, sizeof(use)) == 0;
    nyr_mach_func_free(&m);
    if (!ok) return false;
  }
  return true;
}

int main(void) {
  if (!nyr_mach_schedule_post_ra_selftest() || !vector_load_users()) {
    fputs("[machine-scheduler] safe-segment scheduling regression failed\n",
          stderr);
    return 1;
  }
  puts("[machine-scheduler] pure segments reorder; barriers remain fixed");
  return 0;
}
