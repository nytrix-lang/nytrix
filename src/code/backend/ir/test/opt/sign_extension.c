/*
 * NYIR regression tests for signed integer extension semantics.
 */
#include "support.h"

#include <stdint.h>

static int64_t evaluate(nyr_func_t *f, int64_t input) {
  int64_t locals[8] = {input};
  nyr_eval_result_t result = {0};
  char error[256] = {0};
  if (!nyr_ir_eval(f, locals, 8, 500, &result, error, sizeof(error))) {
    fprintf(stderr, "[sign-extension] evaluation failed: %s\n", error);
    exit(1);
  }
  int64_t value = result.result;
  nyr_eval_result_free(&result);
  return value;
}

int main(void) {
  static const int shifts[] = {1, 16, 32, 48, 56, 63};
  static const int64_t inputs[] = {0,
                                   1,
                                   -1,
                                   127,
                                   128,
                                   -128,
                                   -129,
                                   32767,
                                   32768,
                                   -32768,
                                   -32769,
                                   INT32_MAX,
                                   (int64_t)INT32_MAX + 1,
                                   INT32_MIN,
                                   (int64_t)INT32_MIN - 1,
                                   INT64_MIN,
                                   INT64_MAX,
                                   INT64_C(0x123456789abcdef)};
  for (size_t inner = 0; inner < sizeof(shifts) / sizeof(shifts[0]); ++inner) {
    for (size_t outer = 0; outer < sizeof(shifts) / sizeof(shifts[0]);
         ++outer) {
      nyr_func_t f;
      nyr_opt_begin(&f);
      int input =
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
      int inner_count = nyr_opt_const(&f, shifts[inner]);
      int inner_left = nyr_opt_binop(&f, NYIR_SHL_I64, input, inner_count);
      int extended = nyr_opt_binop(&f, NYIR_SAR_I64, inner_left, inner_count);
      int outer_count = nyr_opt_const(&f, shifts[outer]);
      int outer_left = nyr_opt_binop(&f, NYIR_SHL_I64, extended, outer_count);
      int output = nyr_opt_binop(&f, NYIR_SAR_I64, outer_left, outer_count);
      nyr_opt_ret(&f, output);
      int64_t before[sizeof(inputs) / sizeof(inputs[0])];
      for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i)
        before[i] = evaluate(&f, inputs[i]);
      bool ok = nyr_peephole(&f) && nyr_opt_verify(&f, "sign-extension");
      for (size_t i = 0; ok && i < f.len; ++i) {
        if (f.data[i].dst == output)
          ok = shifts[outer] <= shifts[inner]
                   ? f.data[i].op == NYIR_COPY && f.data[i].a == extended
                   : f.data[i].op == NYIR_SAR_I64;
      }
      for (size_t i = 0; ok && i < sizeof(inputs) / sizeof(inputs[0]); ++i)
        ok = before[i] == evaluate(&f, inputs[i]);
      nyr_ir_func_free(&f);
      if (!ok) {
        fprintf(stderr, "[sign-extension] inner=%d outer=%d failed\n",
                shifts[inner], shifts[outer]);
        return 1;
      }
    }
  }
  return 0;
}
