#include "support.h"

#include <stdint.h>

static const nyr_inst_t *find_def(const nyr_func_t *f, int value) {
  if (!f || value < 0)
    return NULL;
  for (size_t i = 0; i < f->len; ++i)
    if (f->data[i].dst == value)
      return &f->data[i];
  return NULL;
}

static int64_t eval_input(nyr_func_t *f, int64_t input) {
  int64_t locals[8] = {input};
  nyr_eval_result_t result = {0};
  char err[256] = {0};
  if (!nyr_ir_eval(f, locals, 8, 500, &result, err, sizeof(err))) {
    fprintf(stderr, "[strength-reduce] eval failed: %s\n", err);
    exit(1);
  }
  int64_t value = result.result;
  nyr_eval_result_free(&result);
  return value;
}

static void build_scaled_add(nyr_func_t *f, bool extra_scaled_use,
                             int *result_out) {
  nyr_opt_begin(f);
  int x = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                      .dst = -1,
                                      .a = -1,
                                      .b = -1,
                                      .c = -1,
                                      .d = -1,
                                      .e = -1,
                                      .f = -1,
                                      .imm = 0,
                                      .scalar_class = NYIR_SCALAR_I64_RAW});
  int one = nyr_opt_const(f, 1);
  int plus = nyr_opt_binop(f, NYIR_ADD_I64, x, one);
  int shift_one = nyr_opt_const(f, 1);
  int scaled = nyr_opt_binop(f, NYIR_SHL_I64, plus, shift_one);
  int scaled_copy = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_COPY,
                                                .dst = -1,
                                                .a = scaled,
                                                .b = -1,
                                                .c = -1,
                                                .d = -1,
                                                .e = -1,
                                                .f = -1});
  int x_copy = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_COPY,
                                           .dst = -1,
                                           .a = x,
                                           .b = -1,
                                           .c = -1,
                                           .d = -1,
                                           .e = -1,
                                           .f = -1});
  int result = nyr_opt_binop(f, NYIR_ADD_I64, scaled_copy, x_copy);
  if (extra_scaled_use)
    (void)nyr_opt_binop(f, NYIR_ADD_I64, scaled, one);
  nyr_opt_ret(f, result);
  *result_out = result;
}

static bool test_wraparound_reassociation(void) {
  static const int64_t inputs[] = {
      0, 1, -1, 17, -9223372036854775807LL - 1, 9223372036854775807LL};
  nyr_func_t f;
  int result = -1;
  build_scaled_add(&f, false, &result);
  bool ok = nyr_opt_verify(&f, "strength-reduce/before");
  int64_t before[sizeof(inputs) / sizeof(inputs[0])];
  for (size_t i = 0; ok && i < sizeof(inputs) / sizeof(inputs[0]); ++i)
    before[i] = eval_input(&f, inputs[i]);
  ok = ok && nyr_strength_reduce(&f) &&
       nyr_opt_verify(&f, "strength-reduce/after");
  bool saw_reassociated_result = false;
  for (size_t i = 0; i < f.len; ++i) {
    const nyr_inst_t *in = &f.data[i];
    if (in->dst != result || in->op != NYIR_ADD_I64)
      continue;
    const nyr_inst_t *mul = find_def(&f, in->a);
    saw_reassociated_result = mul && mul->op == NYIR_MUL_I64;
    int64_t multiplier = 0, addend = 0;
    if (saw_reassociated_result) {
      const nyr_inst_t *mult = find_def(&f, mul->b);
      const nyr_inst_t *add = find_def(&f, in->b);
      saw_reassociated_result = mult && add && mult->op == NYIR_CONST_I64 &&
                                add->op == NYIR_CONST_I64;
      if (saw_reassociated_result) {
        multiplier = mult->imm;
        addend = add->imm;
        saw_reassociated_result = multiplier == 3 && addend == 2;
      }
    }
    break;
  }
  ok = ok && saw_reassociated_result;
  for (size_t i = 0; ok && i < sizeof(inputs) / sizeof(inputs[0]); ++i)
    ok = before[i] == eval_input(&f, inputs[i]);
  if (!ok) {
    fprintf(stderr, "[strength-reduce] wrapping reassociation failed\n");
    nyr_dump(stderr, &f, "strength-reduce/wrapping");
  }
  nyr_ir_func_free(&f);
  return ok;
}

static bool test_shared_intermediate_is_not_rewritten(void) {
  nyr_func_t f;
  int result = -1;
  build_scaled_add(&f, true, &result);
  int64_t before = eval_input(&f, 123);
  bool ok = nyr_strength_reduce(&f) &&
            nyr_opt_verify(&f, "strength-reduce/shared-intermediate") &&
            eval_input(&f, 123) == before;
  for (size_t i = 0; ok && i < f.len; ++i)
    if (f.data[i].op == NYIR_MUL_I64 && f.data[i].b >= 0 &&
        f.data[i].b < f.next_value) {
      const nyr_inst_t *mult = find_def(&f, f.data[i].b);
      bool is_three = mult && mult->op == NYIR_CONST_I64 && mult->imm == 3;
      if (is_three)
        ok = false;
    }
  if (!ok) {
    fprintf(stderr, "[strength-reduce] shared intermediate should retain "
                    "original expression\n");
    nyr_dump(stderr, &f, "strength-reduce/shared");
  }
  nyr_ir_func_free(&f);
  return ok;
}

static bool test_signed_power_of_two_division_without_nonnegative_proof(void) {
  static const int64_t inputs[] = {INT64_MIN, -17, -3, -1,       0,
                                   1,         3,   17, INT64_MAX};
  nyr_func_t f;
  nyr_opt_begin(&f);
  int dividend =
      nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                   .dst = -1,
                                   .a = -1,
                                   .b = -1,
                                   .c = -1,
                                   .d = -1,
                                   .e = -1,
                                   .f = -1,
                                   .imm = 0,
                                   .scalar_class = NYIR_SCALAR_I64_RAW});
  int two = nyr_opt_const(&f, 2);
  int quotient = nyr_opt_binop(&f, NYIR_DIV_I64, dividend, two);
  nyr_opt_ret(&f, quotient);
  bool ok = nyr_opt_verify(&f, "strength-reduce/signed-div-before");
  int64_t before[sizeof(inputs) / sizeof(inputs[0])];
  for (size_t i = 0; ok && i < sizeof(inputs) / sizeof(inputs[0]); ++i)
    before[i] = eval_input(&f, inputs[i]);
  ok = ok && nyr_strength_reduce(&f) &&
       nyr_opt_verify(&f, "strength-reduce/signed-div-after");
  size_t div_count = 0;
  size_t sar_count = 0;
  size_t and_count = 0;
  size_t add_count = 0;
  for (size_t i = 0; ok && i < f.len; ++i)
    switch (f.data[i].op) {
    case NYIR_DIV_I64:
      ++div_count;
      break;
    case NYIR_CONST_I64:
      break;
    case NYIR_SAR_I64:
      ++sar_count;
      break;
    case NYIR_AND_I64:
      ++and_count;
      break;
    case NYIR_ADD_I64:
      ++add_count;
      break;
    default:
      break;
    }
  ok = ok && div_count == 1 && sar_count == 0 && and_count == 0 &&
       add_count == 0;
  for (size_t i = 0; ok && i < sizeof(inputs) / sizeof(inputs[0]); ++i)
    ok = before[i] == eval_input(&f, inputs[i]);
  if (!ok) {
    fprintf(stderr, "[strength-reduce] signed power-of-two division without "
                    "a nonnegative proof should remain unchanged\n");
    nyr_dump(stderr, &f, "strength-reduce/signed-div");
  }
  nyr_ir_func_free(&f);
  return ok;
}

static bool test_power_of_two_rewrite_preserves_dividend(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int dividend = nyr_opt_const(&f, 12);
  int divisor = nyr_opt_const(&f, 8);
  int quotient = nyr_opt_binop(&f, NYIR_DIV_I64, dividend, divisor);
  int remainder = nyr_opt_binop(&f, NYIR_MOD_I64, dividend, divisor);
  int result = nyr_opt_binop(&f, NYIR_ADD_I64, quotient, remainder);
  nyr_opt_ret(&f, result);

  int64_t before = eval_input(&f, 0);
  bool ok = nyr_opt_verify(&f, "strength-reduce/power-two-before") &&
            nyr_strength_reduce(&f) &&
            nyr_opt_verify(&f, "strength-reduce/power-two-after");
  const nyr_inst_t *quotient_def = find_def(&f, quotient);
  const nyr_inst_t *remainder_def = find_def(&f, remainder);
  ok = ok && quotient_def && quotient_def->op == NYIR_SAR_I64 &&
       quotient_def->a == dividend && remainder_def &&
       remainder_def->op == NYIR_AND_I64 && remainder_def->a == dividend &&
       eval_input(&f, 0) == before;
  if (!ok) {
    fprintf(stderr,
            "[strength-reduce] power-of-two rewrite lost its dividend\n");
    nyr_dump(stderr, &f, "strength-reduce/power-two-operands");
  }
  nyr_ir_func_free(&f);
  return ok;
}

int main(void) {
  if (!test_wraparound_reassociation() ||
      !test_shared_intermediate_is_not_rewritten() ||
      !test_signed_power_of_two_division_without_nonnegative_proof() ||
      !test_power_of_two_rewrite_preserves_dividend())
    return 1;
  puts("[strength-reduce] wrapping reassociation, use checks, and conservative "
       "signed division ok");
  return 0;
}
