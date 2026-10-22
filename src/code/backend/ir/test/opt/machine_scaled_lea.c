#include "code/backend/ir/machine.h"
#include "code/backend/native.h"
#include "support.h"

#include <limits.h>

static bool check_case(nyr_op_t scale_op, int64_t amount_value,
                       int64_t base_value, int64_t index_value,
                       int64_t expected) {
  nyr_func_t ir;
  nyr_mach_func_t mach = {0};
  nyr_opt_begin(&ir);
  int base = nyr_opt_const(&ir, base_value);
  int index = nyr_opt_const(&ir, index_value);
  int amount = nyr_opt_const(&ir, amount_value);
  int scaled = nyr_opt_binop(&ir, scale_op, index, amount);
  int result = nyr_opt_binop(&ir, NYIR_ADD_I64, base, scaled);
  nyr_opt_ret(&ir, result);

  char err[256] = {0};
  bool ok =
      nyr_opt_verify(&ir, "machine-scaled-lea") &&
      nyr_opt_eval(&ir, "machine-scaled-lea") == expected &&
      nyr_mach_lower_nir(&ir, &mach, NY_NATIVE_CAP_MACH_SCALED_LEA, err,
                         sizeof(err)) &&
      nyr_mach_verify(&mach, NY_NATIVE_CAP_MACH_SCALED_LEA, err, sizeof(err));
  size_t scaled_leas = 0, shifts = 0;
  for (size_t i = 0; ok && i < mach.inst_len; ++i) {
    const nyr_mach_inst_t *in = &mach.insts[i];
    if (in->opcode == NYR_MACH_LEA && in->src0.kind == NYR_MACH_OPERAND_VREG &&
        in->src1.kind == NYR_MACH_OPERAND_VREG &&
        in->src2.kind == NYR_MACH_OPERAND_IMM &&
        (in->src2.as.imm == 2 || in->src2.as.imm == 4 || in->src2.as.imm == 8))
      ++scaled_leas;
    if (in->opcode == NYR_MACH_SHL || in->opcode == NYR_MACH_MUL)
      ++shifts;
  }
  ok = ok && scaled_leas == 1 && shifts == 0;
  if (!ok) {
    fprintf(stderr, "[machine-scaled-lea] %s (op=%d scale=%lld)\n",
            err[0] ? err : "distinct base/index scale was not folded",
            (int)scale_op, (long long)amount_value);
    nyr_mach_dump(&mach, stderr);
  }
  nyr_mach_func_free(&mach);
  nyr_ir_func_free(&ir);
  return ok;
}

static bool check_shared_scale(bool cross_block, bool extra_use) {
  nyr_func_t ir;
  nyr_mach_func_t mach = {0};
  nyr_opt_begin(&ir);
  int first_base = nyr_opt_const(&ir, 1000);
  int second_base = nyr_opt_const(&ir, 2000);
  int index = nyr_opt_const(&ir, -5);
  int amount = nyr_opt_const(&ir, 3);
  int scaled = nyr_opt_binop(&ir, NYIR_SHL_I64, index, amount);
  int first = nyr_opt_binop(&ir, NYIR_ADD_I64, first_base, scaled);
  if (cross_block) {
    nyr_opt_br(&ir, 20);
    nyr_opt_label(&ir, 20);
  }
  int second = nyr_opt_binop(&ir, NYIR_ADD_I64, second_base, scaled);
  int result = nyr_opt_binop(&ir, NYIR_ADD_I64, first, second);
  if (extra_use) {
    int extra = nyr_opt_binop(&ir, NYIR_MUL_I64, scaled, scaled);
    result = nyr_opt_binop(&ir, NYIR_ADD_I64, result, extra);
  }
  nyr_opt_ret(&ir, result);

  char err[256] = {0};
  bool ok =
      nyr_opt_verify(&ir, "machine-scaled-lea-shared") &&
      nyr_opt_eval(&ir, "machine-scaled-lea-shared") ==
          (extra_use ? 4520 : 2920) &&
      nyr_mach_lower_nir(&ir, &mach, NY_NATIVE_CAP_MACH_SCALED_LEA, err,
                         sizeof(err)) &&
      nyr_mach_verify(&mach, NY_NATIVE_CAP_MACH_SCALED_LEA, err, sizeof(err));
  size_t scaled_leas = 0, shifts = 0;
  for (size_t i = 0; ok && i < mach.inst_len; ++i) {
    const nyr_mach_inst_t *in = &mach.insts[i];
    if (in->opcode == NYR_MACH_LEA && in->src0.kind == NYR_MACH_OPERAND_VREG &&
        in->src1.kind == NYR_MACH_OPERAND_VREG &&
        in->src2.kind == NYR_MACH_OPERAND_IMM && in->src2.as.imm == 8)
      ++scaled_leas;
    if (in->opcode == NYR_MACH_SHL)
      ++shifts;
  }
  size_t expected_leas = 2;
  size_t expected_shifts = extra_use ? 1 : 0;
  ok = ok && scaled_leas == expected_leas && shifts == expected_shifts;
  if (!ok) {
    fprintf(stderr,
            "[machine-scaled-lea] shared scale failed (cross-block=%d "
            "extra-use=%d): %s\n",
            cross_block, extra_use,
            err[0] ? err : "unexpected LEA or scale instruction count");
    nyr_mach_dump(&mach, stderr);
  }
  nyr_mach_func_free(&mach);
  nyr_ir_func_free(&ir);
  return ok;
}

int main(void) {
  bool ok = check_case(NYIR_SHL_I64, 1, 1000, -5, 990) &&
            check_case(NYIR_SHL_I64, 2, 1000, -5, 980) &&
            check_case(NYIR_SHL_I64, 3, 1000, -5, 960) &&
            check_case(NYIR_SHL_I64, 3, INT64_MAX - 3, 2, INT64_MIN + 12) &&
            check_case(NYIR_MUL_I64, 2, 1000, -5, 990) &&
            check_case(NYIR_MUL_I64, 4, 1000, -5, 980) &&
            check_case(NYIR_MUL_I64, 8, 1000, -5, 960) &&
            check_case(NYIR_MUL_I64, 8, INT64_MAX - 3, 2, INT64_MIN + 12) &&
            check_shared_scale(false, false) &&
            check_shared_scale(true, false) && check_shared_scale(false, true);
  if (ok)
    puts("[machine-scaled-lea] scaled indexes folded; shared-use and CFG "
         "dominance preserved");
  return ok ? 0 : 1;
}
