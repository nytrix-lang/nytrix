/*
 * CMP/JCC flag-liveness regression: preserve flags through neutral ops, reject
 * clobbers.
 */
#include "code/backend/object/internal.h"

#include <stdio.h>
#include <string.h>

static nyr_mach_operand_t vreg(nyr_mach_reg_t reg) {
  return (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_VREG,
                              .reg_class = NYR_MACH_REGCLASS_GPR,
                              .as.reg = reg};
}

static nyr_mach_operand_t block(uint32_t index) {
  return (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_BLOCK,
                              .as.block_index = index};
}

static bool emit(nyr_mach_func_t *mach, nyr_mach_opcode_t opcode,
                 nyr_mach_operand_t dst, nyr_mach_operand_t src0,
                 nyr_mach_operand_t src1, unsigned effects,
                 nyr_mach_cond_t condition) {
  return nyr_mach_emit(mach, (nyr_mach_inst_t){.opcode = opcode,
                                               .dst = dst,
                                               .src0 = src0,
                                               .src1 = src1,
                                               .effects = effects,
                                               .condition = condition});
}

static size_t count_bytes(const nyr_obj_buf_t *code, unsigned char a,
                          unsigned char b) {
  size_t count = 0;
  for (size_t i = 0; i + 1 < code->len; ++i)
    count += code->data[i] == a && code->data[i + 1] == b;
  return count;
}

/*
 * Count `mov reg, disp32(%rbp)` stores emitted by the x64 frame-home path.
 */
static size_t count_frame_home_stores(const nyr_obj_buf_t *code) {
  size_t count = 0;
  for (size_t i = 0; i + 6 < code->len; ++i) {
    bool rex_w = code->data[i] == 0x48 || code->data[i] == 0x49 ||
                 code->data[i] == 0x4c || code->data[i] == 0x4d;
    if (rex_w && code->data[i + 1] == 0x89 &&
        (code->data[i + 2] & 0xc7) == 0x85)
      ++count;
  }
  return count;
}

static bool run_case(const char *name, bool clobber, bool expect_setcc,
                     size_t *frame_stores_out) {
  nyr_mach_func_t mach = {0};
  uint32_t block_index = 0;
  nyr_mach_reg_t lhs, rhs, cmp, tmp, result;
  char err[256] = {0};
  bool ok =
      nyr_mach_begin_block(&mach, 0, &block_index) &&
      nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &lhs) &&
      nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &rhs) &&
      nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &cmp) &&
      nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &tmp) &&
      nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &result) &&
      emit(&mach, NYR_MACH_COPY, vreg(lhs),
           (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_IMM, .as.imm = 7},
           (nyr_mach_operand_t){0}, NYR_MACH_EFFECT_NONE,
           NYR_MACH_COND_ALWAYS) &&
      emit(&mach, NYR_MACH_COPY, vreg(rhs),
           (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_IMM, .as.imm = 7},
           (nyr_mach_operand_t){0}, NYR_MACH_EFFECT_NONE,
           NYR_MACH_COND_ALWAYS) &&
      emit(&mach, NYR_MACH_CMP, vreg(cmp), vreg(lhs), vreg(rhs),
           NYR_MACH_EFFECT_NONE, NYR_MACH_COND_EQ);
  if (ok && clobber)
    ok = emit(&mach, NYR_MACH_ADD, vreg(tmp), vreg(lhs), vreg(rhs),
              NYR_MACH_EFFECT_NONE, NYR_MACH_COND_ALWAYS);
  else if (ok)
    ok = emit(&mach, NYR_MACH_COPY, vreg(tmp), vreg(lhs),
              (nyr_mach_operand_t){0}, NYR_MACH_EFFECT_NONE,
              NYR_MACH_COND_ALWAYS);
  if (ok)
    ok = emit(&mach, NYR_MACH_BR_IF, (nyr_mach_operand_t){0}, vreg(cmp),
              block(1), NYR_MACH_EFFECT_CONTROL, NYR_MACH_COND_ALWAYS) &&
         nyr_mach_begin_block(&mach, 1, &block_index) &&
         emit(&mach, NYR_MACH_COPY, vreg(result), vreg(tmp),
              (nyr_mach_operand_t){0}, NYR_MACH_EFFECT_NONE,
              NYR_MACH_COND_ALWAYS) &&
         emit(&mach, NYR_MACH_RET, (nyr_mach_operand_t){0}, vreg(result),
              (nyr_mach_operand_t){0}, NYR_MACH_EFFECT_CONTROL,
              NYR_MACH_COND_ALWAYS);
  if (ok)
    ok = nyr_mach_verify(&mach, 0, err, sizeof(err));
  if (!ok) {
    fprintf(stderr, "[%s] setup failed: %s\n", name, err);
    nyr_mach_func_free(&mach);
    return false;
  }
  nyr_obj_buf_t code = {0};
  nyr_x64_obj_symbol_def_t defs[1] = {0};
  /*
   * This self-contained branch fixture emits no external relocations. Keep
   * the test table bounded instead of placing the production maximum on stack.
   */
  nyr_x64_obj_reloc_t relocs[8] = {0};
  size_t def_count = 0, reloc_count = 0;
  ny_native_target_info_t target = {
      .target = NY_NATIVE_TARGET_X86_64,
      .abi = NY_NATIVE_ABI_SYSV,
      .symbol_prefix = "",
  };
  ok = nyr_x64_mach_append_function(&code, defs, &def_count, relocs,
                                   &reloc_count, &mach, &target, name, false,
                                   err, sizeof(err));
  size_t setcc = count_bytes(&code, 0x0f, 0x94);
  size_t frame_stores = count_frame_home_stores(&code);
  nyr_obj_free(&code);
  nyr_mach_func_free(&mach);
  if (frame_stores_out)
    *frame_stores_out = frame_stores;
  if (!ok || (setcc != 0) != expect_setcc) {
    fprintf(stderr, "[%s] want setcc=%s got %zu (%s)\n", name,
            expect_setcc ? "present" : "absent", setcc,
            err[0] ? err : "encoded");
    return false;
  }
  return true;
}

int main(void) {
  size_t fused_stores = 0, clobber_stores = 0;
  bool ok = run_case("flag-fuse-valid", false, false, &fused_stores) &&
            run_case("flag-fuse-clobber", true, true, &clobber_stores);
  if (ok && fused_stores >= clobber_stores) {
    fprintf(stderr,
            "[flag-fuse-home] expected fused branch to avoid its condition "
            "home store; fused=%zu unfused=%zu\n",
            fused_stores, clobber_stores);
    ok = false;
  }
  return !ok;
}
