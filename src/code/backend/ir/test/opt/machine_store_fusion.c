/*
 * Regression tests for machine-level store fusion and live pointer values.
 */
#include "code/backend/internal.h"
#include "code/backend/ir/machine.h"
#include "code/backend/native.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static nyr_mach_operand_t reg(nyr_mach_reg_t v) {
  return (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_VREG,
                              .reg_class = NYR_MACH_REGCLASS_GPR,
                              .as.reg = v};
}

static bool check_store(bool byte, bool pointer_live) {
  nyr_mach_func_t m = {0};
  nyr_mach_reg_t value, ptr;
  uint32_t block;
  char err[256] = {0};
  bool ok =
      nyr_mach_alloc_typed_vreg(&m, NYR_MACH_TYPE_I64, &value) &&
      nyr_mach_alloc_typed_vreg(&m, NYR_MACH_TYPE_I64, &ptr) &&
      nyr_mach_begin_block(&m, 0, &block) &&
      nyr_mach_emit(&m, (nyr_mach_inst_t){.opcode = NYR_MACH_COPY,
                                          .dst = reg(value),
                                          .src0 = {.kind = NYR_MACH_OPERAND_IMM,
                                                   .as.imm = 0x1234}}) &&
      nyr_mach_emit(&m,
                    (nyr_mach_inst_t){.opcode = NYR_MACH_LEA,
                                      .dst = reg(ptr),
                                      .src0 = {.kind = NYR_MACH_OPERAND_SYMBOL,
                                               .as.symbol = "fusion_cell"}}) &&
      nyr_mach_emit(
          &m, (nyr_mach_inst_t){.opcode = NYR_MACH_STORE,
                                .dst = reg(ptr),
                                .src0 = reg(value),
                                .byte_width = byte,
                                .effects = NYR_MACH_EFFECT_WRITE_MEMORY}) &&
      nyr_mach_emit(&m,
                    (nyr_mach_inst_t){.opcode = NYR_MACH_RET,
                                      .src0 = reg(pointer_live ? ptr : value),
                                      .effects = NYR_MACH_EFFECT_CONTROL});
  nyr_writer_t w = {0};
  const ny_native_target_info_t target = {.target = NY_NATIVE_TARGET_X86_64,
                                          .abi = NY_NATIVE_ABI_SYSV,
                                          .object_format = "elf",
                                          .symbol_prefix = ""};
  ok = ok && nyr_mach_verify(&m, 0, err, sizeof(err)) &&
       nyr_x86_64_emit_mach_scalar(&w, &target, &m, "store_fusion", false,
                                   false, err, sizeof(err));
  const char *direct =
      byte ? "\tmovb\t%cl, (%rax)\n" : "\tmovq\t%rcx, (%rax)\n";
  if (ok) {
    bool fused = strstr(w.data, direct) != NULL;
    ok = fused == !pointer_live;
  }
  if (!ok)
    fprintf(stderr, "[machine-store-fusion] byte=%d live=%d %s\n%s", byte,
            pointer_live, err, w.data ? w.data : "<no assembly>\n");
  free(w.data);
  nyr_mach_func_free(&m);
  return ok;
}

static bool check_forward(unsigned kind) {
  nyr_mach_func_t m = {0};
  nyr_mach_reg_t a, b, c, d;
  uint32_t block;
  char err[256] = {0};
  bool ok = nyr_mach_alloc_typed_vreg(&m, NYR_MACH_TYPE_I64, &a) &&
            nyr_mach_alloc_typed_vreg(&m, NYR_MACH_TYPE_I64, &b) &&
            nyr_mach_alloc_typed_vreg(&m, NYR_MACH_TYPE_I64, &c) &&
            nyr_mach_alloc_typed_vreg(&m, NYR_MACH_TYPE_I64, &d) &&
            nyr_mach_begin_block(&m, 0, &block);
  if (kind != 1)
    ok = ok && nyr_mach_emit(
                   &m, (nyr_mach_inst_t){.opcode = NYR_MACH_COPY,
                                         .dst = reg(d),
                                         .src0 = {.kind = NYR_MACH_OPERAND_IMM,
                                                  .as.imm = 1}});
  ok = ok && nyr_mach_emit(
                 &m, (nyr_mach_inst_t){
                         .opcode = NYR_MACH_COPY,
                         .dst = reg(a),
                         .src0 = {.kind = NYR_MACH_OPERAND_IMM, .as.imm = 41}});
  nyr_mach_reg_t result = b;
  if (kind == 0) {
    ok = ok && nyr_mach_emit(&m, (nyr_mach_inst_t){.opcode = NYR_MACH_ADD,
                                                   .dst = reg(b),
                                                   .src0 = reg(a),
                                                   .src1 = reg(d)});
  } else if (kind == 1) {
    ok = ok &&
         nyr_mach_emit(&m,
                       (nyr_mach_inst_t){.opcode = NYR_MACH_COPY,
                                         .dst = reg(b),
                                         .src0 = {.kind = NYR_MACH_OPERAND_IMM,
                                                  .as.imm = 42}}) &&
         nyr_mach_emit(&m, (nyr_mach_inst_t){.opcode = NYR_MACH_CMP,
                                             .dst = reg(c),
                                             .src0 = reg(a),
                                             .src1 = reg(b),
                                             .condition = NYR_MACH_COND_LT}) &&
         nyr_mach_emit(&m, (nyr_mach_inst_t){.opcode = NYR_MACH_SELECT,
                                             .dst = reg(d),
                                             .src0 = reg(c),
                                             .src1 = reg(a),
                                             .src2 = reg(b),
                                             .condition = NYR_MACH_COND_NE});
    result = d;
  } else {
    ok = ok &&
         nyr_mach_emit(&m, (nyr_mach_inst_t){.opcode = NYR_MACH_BSWAP64,
                                             .dst = reg(b),
                                             .src0 = reg(a)}) &&
         nyr_mach_emit(&m, (nyr_mach_inst_t){.opcode = NYR_MACH_ADD,
                                             .dst = reg(c),
                                             .src0 = reg(b),
                                             .src1 = reg(d)});
    result = c;
  }
  ok = ok &&
       nyr_mach_emit(&m, (nyr_mach_inst_t){.opcode = NYR_MACH_RET,
                                           .src0 = reg(result),
                                           .effects = NYR_MACH_EFFECT_CONTROL});
  nyr_writer_t w = {0};
  const ny_native_target_info_t target = {.target = NY_NATIVE_TARGET_X86_64,
                                          .abi = NY_NATIVE_ABI_SYSV,
                                          .object_format = "elf",
                                          .symbol_prefix = ""};
  ok = ok && nyr_mach_verify(&m, 0, err, sizeof(err)) &&
       nyr_x86_64_emit_mach_scalar(&w, &target, &m, "scalar_forward", false,
                                   false, err, sizeof(err));
  const char *pattern = kind == 0   ? "\taddq\t$1, %rax\n"
                        : kind == 1 ? "\tcmovl\t"
                                    : "\tbswapq\t%rax\n";
  if (ok)
    ok = strstr(w.data, pattern) != NULL;
  if (!ok)
    fprintf(stderr, "[machine-store-fusion] forward=%u %s\n%s", kind, err,
            w.data ? w.data : "<no assembly>\n");
  free(w.data);
  nyr_mach_func_free(&m);
  return ok;
}

int main(void) {
  bool ok = check_store(false, false) && check_store(true, false) &&
            check_store(false, true) && check_store(true, true) &&
            check_forward(0) && check_forward(1) && check_forward(2);
  if (ok)
    puts("[machine-store-fusion] single-use LEA stores fused; live pointers "
         "preserved");
  return ok ? 0 : 1;
}
