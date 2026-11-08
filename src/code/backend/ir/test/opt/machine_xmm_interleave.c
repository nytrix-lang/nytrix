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

static bool check(bool f32, unsigned op, unsigned n, unsigned barrier) {
  nyr_mach_func_t m = {0};
  nyr_mach_reg_t a, b, result, ptr, tmp, aux, ret, extra;
  nyr_mach_type_t fp = f32 ? NYR_MACH_TYPE_F32 : NYR_MACH_TYPE_F64;
  nyr_mach_operand_t none = {0};
  nyr_mach_operand_t one = {.kind = NYR_MACH_OPERAND_IMM, .as.imm = 1};
  nyr_mach_operand_t ia = {.kind = NYR_MACH_OPERAND_IMM,
                            .as.imm = f32 ? INT64_C(0x41100000)
                                           : INT64_C(0x4022000000000000)};
  nyr_mach_operand_t ib = {.kind = NYR_MACH_OPERAND_IMM,
                            .as.imm = f32 ? INT64_C(0x40000000)
                                           : INT64_C(0x4000000000000000)};
  nyr_mach_operand_t sym = {.kind = NYR_MACH_OPERAND_SYMBOL,
                             .as.symbol = "xmm_interleave_sink"};
  uint32_t entry, second;
  char err[256] = {0};
  bool ok = nyr_mach_alloc_typed_vreg(&m, fp, &a) &&
            nyr_mach_alloc_typed_vreg(&m, fp, &b) &&
            nyr_mach_alloc_typed_vreg(&m, fp, &result) &&
            nyr_mach_alloc_typed_vreg(&m, NYR_MACH_TYPE_PTR, &ptr) &&
            nyr_mach_alloc_typed_vreg(&m, NYR_MACH_TYPE_I64, &tmp) &&
            nyr_mach_alloc_typed_vreg(&m, NYR_MACH_TYPE_I64, &aux) &&
            nyr_mach_alloc_typed_vreg(&m, NYR_MACH_TYPE_I64, &ret) &&
            nyr_mach_alloc_typed_vreg(&m, fp, &extra) &&
            nyr_mach_begin_block(&m, 0, &entry) && entry == 0;
  nyr_mach_operand_t va = vr(a, true), vb = vr(b, true);
  nyr_mach_operand_t vf = vr(result, true), vp = vr(ptr, false);
  nyr_mach_operand_t vt = vr(tmp, false), vu = vr(aux, false);
  nyr_mach_operand_t vz = vr(ret, false);
  nyr_mach_opcode_t opcode = op == 0 ? NYR_MACH_ADD
                              : op == 1 ? NYR_MACH_MUL
                              : op == 2 ? NYR_MACH_DIV : NYR_MACH_SQRT;
  ok = ok && emit(&m, NYR_MACH_COPY, va, ia, none) &&
       emit(&m, NYR_MACH_COPY, vb, ib, none) &&
       emit(&m, opcode, vf, va, opcode == NYR_MACH_SQRT ? none : vb);
  for (unsigned i = 0; ok && i < n; ++i) {
    if (i % 3 == 0)
      ok = emit(&m, NYR_MACH_NOP, none, none, none);
    else {
      nyr_mach_reg_t id;
      ok = nyr_mach_alloc_typed_vreg(&m, NYR_MACH_TYPE_I64, &id);
      if (ok) {
        vu = vr(id, false);
        ok = i % 3 == 1 ? emit(&m, NYR_MACH_COPY, vu, one, none)
                        : emit(&m, NYR_MACH_ADD, vu, vt, vt);
        vt = vu;
      }
    }
  }
  if (barrier == 1) {
    nyr_mach_operand_t target = {.kind = NYR_MACH_OPERAND_BLOCK,
                                 .as.block_index = 1};
    ok = ok && emit(&m, NYR_MACH_BR, none, none, target) &&
         nyr_mach_begin_block(&m, 1, &second) && second == 1;
  } else if (barrier == 2) {
    ok = ok && emit(&m, NYR_MACH_COPY, vr(extra, true), ia, none);
  }
  ok = ok && emit(&m, NYR_MACH_LEA, vp, sym, none) &&
       emit(&m, NYR_MACH_STORE, vp, vf, none) &&
       emit(&m, NYR_MACH_COPY, vz, one, none) &&
       emit(&m, NYR_MACH_RET, none, vz, none);
  nyr_writer_t w = {0};
  const ny_native_target_info_t target = {
      .target = NY_NATIVE_TARGET_X86_64, .abi = NY_NATIVE_ABI_SYSV,
      .object_format = "elf", .symbol_prefix = ""};
  ok = ok && nyr_mach_verify(&m, 0, err, sizeof(err)) &&
       nyr_x86_64_emit_mach_scalar(&w, &target, &m, "xmm_interleave", false, false,
                                   err, sizeof(err));
  if (ok) {
    const char *mnemonic = op == 0 ? f32 ? "\taddss\t" : "\taddsd\t"
                           : op == 1 ? f32 ? "\tmulss\t" : "\tmulsd\t"
                           : op == 2 ? f32 ? "\tdivss\t" : "\tdivsd\t"
                                     : "\tsqrtsd\t";
    const char *producer = strstr(w.data, mnemonic);
    const char *store = producer ? strstr(producer, f32 ? "\tmovss\t%xmm0, (%rax)"
                                                    : "\tmovsd\t%xmm0, (%rax)") : NULL;
    const char *spill = producer ? strstr(producer, f32 ? "\tmovss\t%xmm0, -"
                                                    : "\tmovsd\t%xmm0, -") : NULL;
    bool expect_forward = barrier == 0 && n + 2 <= 32;
    ok = producer && store && ((spill && spill < store) != expect_forward);
  }
  if (!ok)
    fprintf(stderr, "[machine-xmm-interleave] f32=%d op=%u n=%u barrier=%u: %s\n%s\n",
            f32, op, n, barrier, err, w.data ? w.data : "<no asm>");
  free(w.data);
  nyr_mach_func_free(&m);
  return ok;
}

int main(void) {
  bool ok = true;
  unsigned checks = 0;
  for (unsigned fp = 0; fp < 2; ++fp)
    for (unsigned op = 0; op < 4; ++op) {
      if (fp && op == 3)
        continue;
      for (unsigned n = 0; n <= 34; ++n)
        for (unsigned barrier = 0; barrier < 3; ++barrier) {
          ok = check(fp != 0, op, n, barrier) && ok;
          ++checks;
        }
    }
  if (ok)
    printf("[machine-xmm-interleave] %u forwarding/barrier cases passed\n", checks);
  return ok ? 0 : 1;
}
