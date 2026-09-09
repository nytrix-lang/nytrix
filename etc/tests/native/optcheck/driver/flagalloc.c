/*
 * CMP/JCC flag-liveness regression: preserve flags through neutral ops, reject clobbers.
 */
#include "code/native/object/internal.h"

#include <stdio.h>
#include <string.h>

static ny_mach_operand_t vreg(ny_mach_reg_t reg) {
  return (ny_mach_operand_t){.kind = NY_MACH_OPERAND_VREG,
                             .reg_class = NY_MACH_REGCLASS_GPR,
                             .as.reg = reg};
}

static ny_mach_operand_t block(uint32_t index) {
  return (ny_mach_operand_t){.kind = NY_MACH_OPERAND_BLOCK,
                             .as.block_index = index};
}

static bool emit(ny_mach_func_t *mach, ny_mach_opcode_t opcode,
                 ny_mach_operand_t dst, ny_mach_operand_t src0,
                 ny_mach_operand_t src1, unsigned effects,
                 ny_mach_cond_t condition) {
  return ny_mach_emit(mach, (ny_mach_inst_t){.opcode = opcode,
                                              .dst = dst,
                                              .src0 = src0,
                                              .src1 = src1,
                                              .effects = effects,
                                              .condition = condition});
}

static size_t count_bytes(const ny_obj_buf_t *code, unsigned char a,
                          unsigned char b) {
  size_t count = 0;
  for (size_t i = 0; i + 1 < code->len; ++i)
    count += code->data[i] == a && code->data[i + 1] == b;
  return count;
}

static bool run_case(const char *name, bool clobber, bool expect_setcc) {
  ny_mach_func_t mach = {0};
  uint32_t block_index = 0;
  ny_mach_reg_t lhs, rhs, cmp, tmp, result;
  char err[256] = {0};
  bool ok = ny_mach_begin_block(&mach, 0, &block_index) &&
            ny_mach_alloc_typed_vreg(&mach, NY_MACH_TYPE_I64, &lhs) &&
            ny_mach_alloc_typed_vreg(&mach, NY_MACH_TYPE_I64, &rhs) &&
            ny_mach_alloc_typed_vreg(&mach, NY_MACH_TYPE_I64, &cmp) &&
            ny_mach_alloc_typed_vreg(&mach, NY_MACH_TYPE_I64, &tmp) &&
            ny_mach_alloc_typed_vreg(&mach, NY_MACH_TYPE_I64, &result) &&
            emit(&mach, NY_MACH_COPY, vreg(lhs),
                 (ny_mach_operand_t){.kind = NY_MACH_OPERAND_IMM, .as.imm = 7},
                 (ny_mach_operand_t){0}, NY_MACH_EFFECT_NONE,
                 NY_MACH_COND_ALWAYS) &&
            emit(&mach, NY_MACH_COPY, vreg(rhs),
                 (ny_mach_operand_t){.kind = NY_MACH_OPERAND_IMM, .as.imm = 7},
                 (ny_mach_operand_t){0}, NY_MACH_EFFECT_NONE,
                 NY_MACH_COND_ALWAYS) &&
            emit(&mach, NY_MACH_CMP, vreg(cmp), vreg(lhs), vreg(rhs),
                 NY_MACH_EFFECT_NONE, NY_MACH_COND_EQ);
  if (ok && clobber)
    ok = emit(&mach, NY_MACH_ADD, vreg(tmp), vreg(lhs), vreg(rhs),
              NY_MACH_EFFECT_NONE, NY_MACH_COND_ALWAYS);
  else if (ok)
    ok = emit(&mach, NY_MACH_COPY, vreg(tmp), vreg(lhs),
              (ny_mach_operand_t){0}, NY_MACH_EFFECT_NONE,
              NY_MACH_COND_ALWAYS);
  if (ok)
    ok = emit(&mach, NY_MACH_BR_IF, (ny_mach_operand_t){0}, vreg(cmp),
              block(1), NY_MACH_EFFECT_CONTROL, NY_MACH_COND_ALWAYS) &&
         ny_mach_begin_block(&mach, 1, &block_index) &&
         emit(&mach, NY_MACH_COPY, vreg(result), vreg(tmp),
              (ny_mach_operand_t){0}, NY_MACH_EFFECT_NONE,
              NY_MACH_COND_ALWAYS) &&
         emit(&mach, NY_MACH_RET, (ny_mach_operand_t){0}, vreg(result),
              (ny_mach_operand_t){0}, NY_MACH_EFFECT_CONTROL,
              NY_MACH_COND_ALWAYS);
  if (ok)
    ok = ny_mach_verify(&mach, 0, err, sizeof(err));
  if (!ok) {
    fprintf(stderr, "[%s] setup failed: %s\n", name, err);
    ny_mach_func_free(&mach);
    return false;
  }
  ny_obj_buf_t code = {0};
  ny_x64_obj_symbol_def_t defs[1] = {0};
  ny_x64_obj_reloc_t relocs[NY_X64_OBJ_MAX_RELOCS] = {0};
  size_t def_count = 0, reloc_count = 0;
  ny_native_target_info_t target = {
      .target = NY_NATIVE_TARGET_X86_64,
      .abi = NY_NATIVE_ABI_SYSV,
      .symbol_prefix = "",
  };
  ok = ny_x64_mach_append_function(&code, defs, &def_count, relocs,
                                   &reloc_count, &mach, &target, name, false,
                                   err, sizeof(err));
  size_t setcc = count_bytes(&code, 0x0f, 0x94);
  ny_obj_free(&code);
  ny_mach_func_free(&mach);
  if (!ok || (setcc != 0) != expect_setcc) {
    fprintf(stderr, "[%s] want setcc=%s got %zu (%s)\n", name,
            expect_setcc ? "present" : "absent", setcc,
            err[0] ? err : "encoded");
    return false;
  }
  return true;
}

int main(void) {
  return !(run_case("flag-fuse-valid", false, false) &&
           run_case("flag-fuse-clobber", true, true));
}
