/*
 * A branch that does not overwrite a parameter must retain its incoming value.
 */
#include "support.h"

static bool evaluate(nyr_func_t *f, int64_t expected) {
  int64_t locals[1] = {7};
  nyr_eval_result_t result = {0};
  char err[256] = {0};
  bool ok = nyr_ir_eval(f, locals, 1, 1000, &result, err, sizeof(err)) &&
            result.returned && result.result == expected;
  if (!ok)
    fprintf(stderr, "parameter expected=%lld got=%lld error=%s\n",
            (long long)expected, (long long)result.result, err);
  nyr_eval_result_free(&result);
  return ok;
}

int main(void) {
  for (int cond = 0; cond <= 1; ++cond) {
    nyr_func_t f;
    nyr_opt_begin(&f);
    f.param_types = malloc(sizeof(*f.param_types));
    if (!f.param_types)
      return 2;
    f.param_types[0] = NYIR_PARAM_I64;
    f.param_count = 1;
    int branch = nyr_opt_const(&f, cond), nine = nyr_opt_const(&f, 9);
    nyr_opt_br_if(&f, branch, 10);
    nyr_opt_br(&f, 30);
    nyr_opt_label(&f, 10);
    nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_STORE_LOCAL,
                                .dst = -1,
                                .a = nine,
                                .b = -1,
                                .c = -1,
                                .d = -1,
                                .e = -1,
                                .f = -1,
                                .imm = 0});
    nyr_opt_br(&f, 30);
    nyr_opt_label(&f, 30);
    int value = f.next_value++;
    nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                .dst = value,
                                .a = -1,
                                .b = -1,
                                .c = -1,
                                .d = -1,
                                .e = -1,
                                .f = -1,
                                .imm = 0});
    nyr_opt_ret(&f, value);
    int64_t expected = cond ? 9 : 7;
    bool ok = nyr_opt_verify(&f, "parameter/before") &&
              evaluate(&f, expected) && nyr_mem2reg(&f) &&
              nyr_opt_verify(&f, "parameter/after") && evaluate(&f, expected);
    nyr_ir_func_free(&f);
    if (!ok)
      return 1;
  }
  return 0;
}
