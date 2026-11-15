/*
 * Regression tests for x86 XMM value forwarding across machine instructions.
 */
#include "code/backend/core/internal.h"
#include "code/backend/lower/machine.h"
#include "code/backend/lower/native.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static nyr_mach_operand_t vreg(nyr_mach_reg_t r, bool fp) {
  return (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_VREG,
                              .reg_class = fp ? NYR_MACH_REGCLASS_FPR
                                              : NYR_MACH_REGCLASS_GPR,
                              .as.reg = r};
}

static nyr_mach_operand_t frame(uint32_t index) {
  return (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_FRAME,
                              .as.frame_index = index};
}

static bool emit(nyr_mach_func_t *m, nyr_mach_opcode_t op,
                 nyr_mach_operand_t dst, nyr_mach_operand_t src,
                 nyr_mach_operand_t rhs) {
  return nyr_mach_emit(
      m, (nyr_mach_inst_t){
             .opcode = op,
             .dst = dst,
             .src0 = src,
             .src1 = rhs,
             .effects = op == NYR_MACH_STORE  ? NYR_MACH_EFFECT_WRITE_MEMORY
                        : op == NYR_MACH_LOAD ? NYR_MACH_EFFECT_READ_MEMORY
                        : op == NYR_MACH_RET || op == NYR_MACH_BR
                            ? NYR_MACH_EFFECT_CONTROL
                            : NYR_MACH_EFFECT_NONE});
}

static size_t occurrences(const char *text, const char *pattern) {
  size_t count = 0, length = strlen(pattern);
  for (const char *p = text; (p = strstr(p, pattern)); p += length)
    ++count;
  return count;
}

static bool check(nyr_mach_type_t type, unsigned mode) {
  nyr_mach_func_t m = {0};
  nyr_mach_reg_t a, b, sum, addr, ret, copy;
  uint32_t entry, second, f;
  char err[256] = {0};
  const bool fp32 = type == NYR_MACH_TYPE_F32;
  const int64_t x = fp32 ? 0x3fc00000 : INT64_C(0x3ff8000000000000);
  const int64_t y = fp32 ? 0x40100000 : INT64_C(0x4002000000000000);
  bool ok = nyr_mach_alloc_typed_vreg(&m, type, &a) &&
            nyr_mach_alloc_typed_vreg(&m, type, &b) &&
            nyr_mach_alloc_typed_vreg(&m, type, &sum) &&
            nyr_mach_alloc_typed_vreg(&m, type, &copy) &&
            nyr_mach_alloc_typed_vreg(&m, NYR_MACH_TYPE_PTR, &addr) &&
            nyr_mach_alloc_typed_vreg(&m, NYR_MACH_TYPE_I64, &ret) &&
            nyr_mach_alloc_typed_frame_slot(&m, 8, 8, type, false, &f) &&
            nyr_mach_begin_block(&m, 0, &entry) && entry == 0;
  nyr_mach_operand_t none = {0};
  nyr_mach_operand_t va = vreg(a, true), vb = vreg(b, true);
  nyr_mach_operand_t vs = vreg(sum, true), vc = vreg(copy, true);
  nyr_mach_operand_t vp = vreg(addr, false);
  nyr_mach_operand_t vr = vreg(ret, false);
  nyr_mach_operand_t ix = {.kind = NYR_MACH_OPERAND_IMM, .as.imm = x};
  nyr_mach_operand_t iy = {.kind = NYR_MACH_OPERAND_IMM, .as.imm = y};
  nyr_mach_operand_t zero = {.kind = NYR_MACH_OPERAND_IMM, .as.imm = 0};
  nyr_mach_operand_t symbol = {.kind = NYR_MACH_OPERAND_SYMBOL,
                               .as.symbol = "xmm_forward_cell"};
  ok = ok && emit(&m, NYR_MACH_COPY, vb, iy, none);
  if (mode == 3)
    ok = ok && emit(&m, NYR_MACH_LOAD, va, frame(f), none);
  else
    ok = ok && emit(&m, NYR_MACH_COPY, va, ix, none);
  if (mode != 1 && mode != 8 && mode != 9)
    ok = ok && emit(&m, NYR_MACH_LEA, vp, symbol, none);
  if (mode == 7 || mode == 8)
    ok = ok && emit(&m, NYR_MACH_SQRT, vs, va, none);
  else
    ok = ok && emit(&m, NYR_MACH_ADD, vs, va, vb);
  if (mode == 1 || mode == 8 || mode == 9)
    ok = ok && emit(&m, NYR_MACH_LEA, vp, symbol, none);
  if (mode == 5 || mode == 6)
    ok = ok && emit(&m, NYR_MACH_COPY, vc, vs, none);
  if (mode == 4) {
    nyr_mach_operand_t target = {.kind = NYR_MACH_OPERAND_BLOCK,
                                 .as.block_index = 1};
    ok = ok && emit(&m, NYR_MACH_BR, none, none, target) &&
         nyr_mach_begin_block(&m, 1, &second) && second == 1;
  }
  nyr_mach_operand_t val = mode == 5 || mode == 6 ? vc : vs;
  ok = ok && emit(&m, NYR_MACH_STORE, vp, val, none);
  if (mode == 2 || mode == 6 || mode == 9)
    ok = ok && emit(&m, NYR_MACH_STORE, vp, val, none);
  ok = ok && emit(&m, NYR_MACH_COPY, vr, zero, none) &&
       emit(&m, NYR_MACH_RET, none, vr, none);
  nyr_writer_t w = {0};
  const ny_native_target_info_t target = {.target = NY_NATIVE_TARGET_X86_64,
                                          .abi = NY_NATIVE_ABI_SYSV,
                                          .object_format = "elf",
                                          .symbol_prefix = ""};
  ok = ok && nyr_mach_verify(&m, 0, err, sizeof(err)) &&
       nyr_x86_64_emit_mach_scalar(&w, &target, &m, "xmm_forward", false, false,
                                   err, sizeof(err));
  if (ok) {
    const char *op = mode == 7 || mode == 8 ? "\tsqrtsd\t"
                     : fp32                 ? "\taddss\t"
                                            : "\taddsd\t";
    const char *spill = fp32 ? "\tmovss\t%xmm0, -" : "\tmovsd\t%xmm0, -";
    const char *add = strstr(w.data, op);
    const char *after = add ? strstr(add, spill) : NULL;
    const char *store = add ? strstr(add, fp32 ? "\tmovss\t%xmm0, (%rax)"
                                               : "\tmovsd\t%xmm0, (%rax)")
                            : NULL;
    ok = add && store &&
         ((after == NULL || after > store) ==
          (mode != 2 && mode != 4 && mode != 6 && mode != 9));
    if (mode == 2 || mode == 4 || mode == 6)
      ok = ok && occurrences(w.data, spill) >= 2;
    if (mode == 9)
      ok = ok && occurrences(w.data, spill) >= 2;
    if (mode == 1 || mode == 8 || mode == 9) {
      const char *lea = strstr(w.data, "\tleaq\txmm_forward_cell");
      const char *ptr_spill = lea ? strstr(lea, "\tmovq\t%rax, -") : NULL;
      ok = ok && lea && ((ptr_spill && ptr_spill < store) == (mode == 9));
    }
  }
  if (!ok)
    fprintf(stderr, "[machine-xmm-forward] type=%u mode=%u %s\n%s", type, mode,
            err, w.data ? w.data : "<no asm>\n");
  free(w.data);
  nyr_mach_func_free(&m);
  return ok;
}

int main(void) {
  bool ok = true;
  for (unsigned type = 0; type < 2; ++type)
    for (unsigned mode = 0; mode < 10; ++mode) {
      if (type && (mode == 7 || mode == 8))
        continue;
      ok = check(type ? NYR_MACH_TYPE_F32 : NYR_MACH_TYPE_F64, mode) && ok;
    }
  if (ok)
    puts("[machine-xmm-forward] eighteen f32/f64 forwarding and barrier cases "
         "passed");
  return ok ? 0 : 1;
}
