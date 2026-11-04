/*
 * Function-wide return ABI checks must also permit distinct integer words.
 */
#include "code/backend/ir/opt/util.h"
#include "support.h"

static int return_value(nyr_func_t *f, nyr_scalar_class_t cls, int value,
                        int64_t *expected) {
  nyr_inst_t in = nyr_opt_inst(NYIR_CONST_I64);
  in.imm = value;
  if (cls == NYIR_SCALAR_F64) {
    double number = value;
    in.op = NYIR_CONST_F64;
    memcpy(&in.imm, &number, sizeof(number));
  } else if (cls == NYIR_SCALAR_F32) {
    float number = value;
    uint32_t bits;
    memcpy(&bits, &number, sizeof(bits));
    in.op = NYIR_CONST_F32;
    in.imm = bits;
  }
  *expected = in.imm;
  return nyr_ir_emit(f, in);
}

static void emit_return(nyr_func_t *f, int value, nyr_scalar_class_t cls) {
  nyr_inst_t in = nyr_opt_inst(NYIR_RET);
  in.a = value;
  in.scalar_class = cls;
  nyr_ir_emit(f, in);
}

static bool check_pair(nyr_scalar_class_t left, nyr_scalar_class_t right,
                       bool accepted) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int condition = nyr_opt_emit_imm(&f, NYIR_LOAD_LOCAL, 0);
  int64_t expected[2];
  nyr_opt_br_if(&f, condition, 1);
  int value = return_value(&f, left, 42, &expected[0]);
  emit_return(&f, value, left);
  nyr_opt_label(&f, 1);
  value = return_value(&f, right, 84, &expected[1]);
  emit_return(&f, value, right);
  char err[256] = {0};
  bool verified = nyr_ir_verify(&f, err, sizeof(err));
  bool ok = verified == accepted;
  if (!accepted)
    ok = ok && strstr(err, "function return ABI differs") &&
         strstr(err, "instruction") && strstr(err, "this return uses");
  for (int path = 0; ok && accepted && path < 2; ++path) {
    int64_t locals[] = {path}, result = -1;
    ok = nyr_opt_eval_locals(&f, "return-abi", locals, 1, &result) &&
         result == expected[path];
  }
  if (!ok)
    fprintf(stderr,
            "[return-abi] left=%s right=%s accepted=%d verified=%d: %s\n",
            nyr_scalar_class_name(left), nyr_scalar_class_name(right), accepted,
            verified, err);
  nyr_ir_func_free(&f);
  return ok;
}

int main(void) {
  const nyr_scalar_class_t words[] = {
      NYIR_SCALAR_UNKNOWN, NYIR_SCALAR_I64_RAW,      NYIR_SCALAR_I64_TAGGED,
      NYIR_SCALAR_STRING,  NYIR_SCALAR_TYPED_BUFFER, NYIR_SCALAR_OBJECT,
      NYIR_SCALAR_POINTER, NYIR_SCALAR_CLOSURE};
  for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); ++i) {
    for (size_t j = 0; j < sizeof(words) / sizeof(words[0]); ++j)
      if (!check_pair(words[i], words[j], true))
        return 1;
    if (!check_pair(words[i], NYIR_SCALAR_F32, false) ||
        !check_pair(NYIR_SCALAR_F32, words[i], false) ||
        !check_pair(words[i], NYIR_SCALAR_F64, false) ||
        !check_pair(NYIR_SCALAR_F64, words[i], false))
      return 1;
  }
  if (!check_pair(NYIR_SCALAR_F32, NYIR_SCALAR_F32, true) ||
      !check_pair(NYIR_SCALAR_F64, NYIR_SCALAR_F64, true) ||
      !check_pair(NYIR_SCALAR_F32, NYIR_SCALAR_F64, false) ||
      !check_pair(NYIR_SCALAR_F64, NYIR_SCALAR_F32, false))
    return 1;
  puts("[return-abi] compatible paths execute; mixed conventions are rejected");
  return 0;
}
