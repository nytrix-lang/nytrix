/*
 * Fault injection for the machine-IR verifier (`nyr_mach_verify`).
 *
 * The NYIR suite (`invariants.c`) covers the typed IR. This is the same
 * contract one layer down: register allocation, instruction selection and the
 * object encoder all trust machine IR, so a function that reaches them with an
 * undefined vreg, a dangling block target or no terminator must be rejected
 * before encoding rather than after.
 *
 * Each case builds one valid function, corrupts a single structural property,
 * and requires rejection. The control must be accepted, and its operand types
 * are deliberately mixed-class so a verifier that ignores register classes
 * cannot pass by accident.
 */
#include "code/backend/lower/machine.h"
#include "opt.h"

static int failures;

static nyr_mach_operand_t vreg(nyr_mach_reg_t reg, nyr_mach_reg_class_t cls) {
  return (nyr_mach_operand_t){
      .kind = NYR_MACH_OPERAND_VREG, .reg_class = cls, .as.reg = reg};
}

static nyr_mach_operand_t imm(int64_t value) {
  return (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_IMM, .as.imm = value};
}

static nyr_mach_operand_t block(size_t index) {
  return (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_BLOCK,
                              .as.block_index = (uint32_t)index};
}

static bool emit(nyr_mach_func_t *mach, nyr_mach_opcode_t opcode,
                 nyr_mach_operand_t dst, nyr_mach_operand_t src0,
                 nyr_mach_operand_t src1) {
  return nyr_mach_emit(mach, (nyr_mach_inst_t){
                                 .opcode = opcode,
                                 .dst = dst,
                                 .src0 = src0,
                                 .src1 = src1,
                                 .effects = (opcode == NYR_MACH_BR ||
                                             opcode == NYR_MACH_RET)
                                                ? NYR_MACH_EFFECT_CONTROL
                                                : NYR_MACH_EFFECT_NONE});
}

/*
 * v0 = 0; v1 = v0 + v0; return v1
 */
static bool build_valid(nyr_mach_func_t *mach, nyr_mach_reg_t *out) {
  uint32_t entry = 0;
  nyr_mach_reg_t v0 = NYR_MACH_REG_INVALID, v1 = NYR_MACH_REG_INVALID;
  if (!nyr_mach_alloc_typed_vreg(mach, NYR_MACH_TYPE_I64, &v0) ||
      !nyr_mach_alloc_typed_vreg(mach, NYR_MACH_TYPE_I64, &v1) ||
      !nyr_mach_begin_block(mach, 0, &entry) || entry != 0)
    return false;
  if (!emit(mach, NYR_MACH_COPY, vreg(v0, NYR_MACH_REGCLASS_GPR), imm(0),
            (nyr_mach_operand_t){0}) ||
      !emit(mach, NYR_MACH_ADD, vreg(v1, NYR_MACH_REGCLASS_GPR),
            vreg(v0, NYR_MACH_REGCLASS_GPR),
            vreg(v0, NYR_MACH_REGCLASS_GPR)) ||
      !emit(mach, NYR_MACH_RET, (nyr_mach_operand_t){0},
            vreg(v1, NYR_MACH_REGCLASS_GPR), (nyr_mach_operand_t){0}))
    return false;
  if (out)
    *out = v1;
  return true;
}

static bool verify_mach(nyr_mach_func_t *mach, char *err, size_t len) {
  return nyr_mach_verify(mach, 0, err, len);
}

static void expect(const char *what, nyr_mach_func_t *mach, bool want) {
  char err[256] = {0};
  bool accepted = verify_mach(mach, err, sizeof(err));
  if (accepted == want) {
    if (!want)
      printf("[mach-invariants] %s: rejected (%s)\n", what,
             err[0] ? err : "no detail");
    return;
  }
  fprintf(stderr, "[mach-invariants] %s: verifier %s, expected %s%s%s\n", what,
          accepted ? "accepted" : "rejected", want ? "accept" : "reject",
          err[0] ? "; said: " : "", err);
  ++failures;
}

int main(void) {
  {
    nyr_mach_func_t mach = {0};
    if (!build_valid(&mach, NULL)) {
      fprintf(stderr, "[mach-invariants] could not build the control\n");
      return 1;
    }
    expect("control", &mach, true);
    nyr_mach_func_free(&mach);
  }
  {
    /*
     * Operand names a vreg the function never allocated.
     */
    nyr_mach_func_t mach = {0};
    nyr_mach_reg_t v1 = NYR_MACH_REG_INVALID;
    build_valid(&mach, &v1);
    mach.insts[2].src0 = vreg(999, NYR_MACH_REGCLASS_GPR);
    expect("undefined vreg operand", &mach, false);
    nyr_mach_func_free(&mach);
  }
  {
    /*
     * Register class disagrees with the vreg's declared type.
     */
    nyr_mach_func_t mach = {0};
    build_valid(&mach, NULL);
    mach.insts[1].src0 = vreg(mach.insts[1].src0.as.reg, NYR_MACH_REGCLASS_FPR);
    expect("wrong register class", &mach, false);
    nyr_mach_func_free(&mach);
  }
  {
    /*
     * Entry block falls off the end without RET or BR.
     */
    nyr_mach_func_t mach = {0};
    build_valid(&mach, NULL);
    mach.insts[mach.inst_len - 1].opcode = NYR_MACH_NOP;
    expect("missing terminator", &mach, false);
    nyr_mach_func_free(&mach);
  }
  {
    /*
     * Branch target names a block that does not exist.
     */
    nyr_mach_func_t mach = {0};
    build_valid(&mach, NULL);
    nyr_mach_reg_t v1 = NYR_MACH_REG_INVALID;
    nyr_mach_func_free(&mach);
    mach = (nyr_mach_func_t){0};
    uint32_t entry = 0;
    nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &v1);
    nyr_mach_begin_block(&mach, 0, &entry);
    emit(&mach, NYR_MACH_BR, (nyr_mach_operand_t){0},
         (nyr_mach_operand_t){0}, block(9));
    expect("branch to a missing block", &mach, false);
    nyr_mach_func_free(&mach);
  }
  {
    /*
     * Two blocks claiming the same label.
     */
    nyr_mach_func_t mach = {0};
    uint32_t first = 0, second = 0;
    bool built = nyr_mach_begin_block(&mach, 4, &first) && first == 0;
    bool duplicate = built && nyr_mach_begin_block(&mach, 4, &second);
    if (duplicate) {
      emit(&mach, NYR_MACH_RET, (nyr_mach_operand_t){0}, imm(0),
           (nyr_mach_operand_t){0});
      expect("duplicate block label", &mach, false);
    } else {
      printf("[mach-invariants] duplicate block label: rejected at "
             "construction\n");
    }
    nyr_mach_func_free(&mach);
  }
  if (failures) {
    fprintf(stderr, "[mach-invariants] %d injected defect(s) escaped the "
                    "machine verifier\n",
            failures);
    return 1;
  }
  printf("[mach-invariants] control accepted, every injected machine-IR defect "
         "rejected\n");
  return 0;
}
