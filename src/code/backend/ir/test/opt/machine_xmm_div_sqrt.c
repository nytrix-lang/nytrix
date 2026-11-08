#include "code/backend/internal.h"
#include "code/backend/ir/machine.h"
#include "code/backend/native.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static nyr_mach_operand_t vr(nyr_mach_reg_t id, bool fp) {
  return (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_VREG,
                              .reg_class = fp ? NYR_MACH_REGCLASS_FPR
                                              : NYR_MACH_REGCLASS_GPR,
                              .as.reg = id};
}

static bool emit(nyr_mach_func_t *m, nyr_mach_opcode_t op,
                 nyr_mach_operand_t dst, nyr_mach_operand_t a,
                 nyr_mach_operand_t b) {
  return nyr_mach_emit(m, (nyr_mach_inst_t){
      .opcode = op, .dst = dst, .src0 = a, .src1 = b,
      .effects = op == NYR_MACH_STORE ? NYR_MACH_EFFECT_WRITE_MEMORY
                 : op == NYR_MACH_BR || op == NYR_MACH_RET
                     ? NYR_MACH_EFFECT_CONTROL : NYR_MACH_EFFECT_NONE});
}

static bool check(bool f32, unsigned mode) {
  nyr_mach_func_t m = {0};
  nyr_mach_reg_t a, b, x, y, ptr, ret;
  nyr_mach_type_t type = f32 ? NYR_MACH_TYPE_F32 : NYR_MACH_TYPE_F64;
  uint32_t entry, second;
  char err[256] = {0};
  nyr_mach_operand_t none = {0};
  nyr_mach_operand_t ia = {.kind = NYR_MACH_OPERAND_IMM,
                           .as.imm = f32 ? 0x41100000 : INT64_C(0x4022000000000000)};
  nyr_mach_operand_t ib = {.kind = NYR_MACH_OPERAND_IMM,
                           .as.imm = f32 ? 0x40000000 : INT64_C(0x4000000000000000)};
  nyr_mach_operand_t zero = {.kind = NYR_MACH_OPERAND_IMM, .as.imm = 0};
  nyr_mach_operand_t sym = {.kind = NYR_MACH_OPERAND_SYMBOL,
                            .as.symbol = "ny_xmm_div_sink"};
  bool ok = nyr_mach_alloc_typed_vreg(&m, type, &a) &&
            nyr_mach_alloc_typed_vreg(&m, type, &b) &&
            nyr_mach_alloc_typed_vreg(&m, type, &x) &&
            nyr_mach_alloc_typed_vreg(&m, type, &y) &&
            nyr_mach_alloc_typed_vreg(&m, NYR_MACH_TYPE_PTR, &ptr) &&
            nyr_mach_alloc_typed_vreg(&m, NYR_MACH_TYPE_I64, &ret) &&
            nyr_mach_begin_block(&m, 0, &entry) && entry == 0;
  nyr_mach_operand_t va = vr(a, true), vb = vr(b, true), vx = vr(x, true);
  nyr_mach_operand_t vy = vr(y, true), vp = vr(ptr, false), vret = vr(ret, false);
  ok = ok && emit(&m, NYR_MACH_COPY, va, ia, none) &&
       emit(&m, NYR_MACH_COPY, vb, ib, none);
  if (mode != 8)
    ok = ok && emit(&m, NYR_MACH_LEA, vp, sym, none);
  nyr_mach_opcode_t first = mode == 2 || mode == 3 ? NYR_MACH_ADD
                            : mode == 5 ? NYR_MACH_SQRT : NYR_MACH_DIV;
  ok = ok && emit(&m, first, vx, va,
                  first == NYR_MACH_SQRT ? none : vb);
  nyr_mach_operand_t value = vx;
  if (mode == 1 || mode == 2 || mode == 3 || mode == 4 || mode == 5 ||
      mode == 9) {
    nyr_mach_opcode_t second_op = mode == 1 ? NYR_MACH_ADD
                                  : mode == 2 || mode == 5 ? NYR_MACH_DIV
                                  : mode == 9 ? NYR_MACH_COPY : NYR_MACH_SQRT;
    ok = ok && emit(&m, second_op, vy, vx,
                    second_op == NYR_MACH_DIV || second_op == NYR_MACH_ADD
                        ? vb : none);
    value = vy;
  }
  if (mode == 8)
    ok = ok && emit(&m, NYR_MACH_LEA, vp, sym, none);
  if (mode == 6) {
    nyr_mach_operand_t target = {.kind = NYR_MACH_OPERAND_BLOCK,
                                 .as.block_index = 1};
    ok = ok && emit(&m, NYR_MACH_BR, none, none, target) &&
         nyr_mach_begin_block(&m, 1, &second) && second == 1;
  }
  ok = ok && emit(&m, NYR_MACH_STORE, vp, value, none);
  if (mode == 7)
    ok = ok && emit(&m, NYR_MACH_STORE, vp, value, none);
  ok = ok && emit(&m, NYR_MACH_COPY, vret, zero, none) &&
       emit(&m, NYR_MACH_RET, none, vret, none);
  nyr_writer_t w = {0};
  const ny_native_target_info_t target = {
      .target = NY_NATIVE_TARGET_X86_64, .abi = NY_NATIVE_ABI_SYSV,
      .object_format = "elf", .symbol_prefix = ""};
  ok = ok && nyr_mach_verify(&m, 0, err, sizeof(err)) &&
       nyr_x86_64_emit_mach_scalar(&w, &target, &m, "xmm_div", false, false,
                                   err, sizeof(err));
  if (ok) {
    const char *first_op = first == NYR_MACH_ADD ? f32 ? "\taddss\t" : "\taddsd\t"
                           : first == NYR_MACH_SQRT ? "\tsqrtsd\t"
                           : f32 ? "\tdivss\t" : "\tdivsd\t";
    const char *start = strstr(w.data, first_op);
    const char *spill = f32 ? "\tmovss\t%xmm0, -" : "\tmovsd\t%xmm0, -";
    const char *store = start ? strstr(start, f32 ? "\tmovss\t%xmm0, (%rax)"
                                                  : "\tmovsd\t%xmm0, (%rax)") : NULL;
    const char *saved = start ? strstr(start, spill) : NULL;
    bool should_spill = mode == 6 || mode == 7;
    ok = start && store && ((saved && saved < store) == should_spill);
    if (ok && (mode == 1 || mode == 2 || mode == 3 || mode == 4 || mode == 5)) {
      const char *second_op = mode == 1 ? f32 ? "\taddss\t" : "\taddsd\t"
                              : mode == 2 || mode == 5 ? f32 ? "\tdivss\t" : "\tdivsd\t"
                              : "\tsqrtsd\t";
      const char *next = strstr(start + strlen(first_op), second_op);
      ok = next && (!saved || saved > next);
    }
  }
  if (!ok)
    fprintf(stderr, "[xmm-div-sqrt] type=%s mode=%u: %s\n%s\n",
            f32 ? "f32" : "f64", mode, err,
            w.data ? w.data : "<no assembly>");
  free(w.data);
  nyr_mach_func_free(&m);
  return ok;
}

int main(void) {
  bool ok = true;
  for (unsigned type = 0; type < 2; ++type)
    for (unsigned mode = 0; mode < 10; ++mode) {
      if (type && (mode == 3 || mode == 4 || mode == 5))
        continue;
      ok = check(type != 0, mode) && ok;
    }
  if (ok)
    puts("[xmm-div-sqrt] seventeen forwarding and barrier cases passed");
  return ok ? 0 : 1;
}
