/*
 * Exact-diamond SELECT regression: semantic equivalence and safety gate.
 */
#include "util.h"

static void build_diamond(nyir_func_t *f, int64_t right, bool side_effect) {
  ny_begin(f);
  int left = ny_const(f, 4);
  int rhs = ny_const(f, right);
  int one = ny_const(f, 1);
  int two = ny_const(f, 2);
  int cond = ny_cmp(f, left, rhs, NYIR_CMP_EQ);
  ny_br_if(f, cond, 20);
  ny_label(f, 10);
  int false_value = ny_binop(f, NYIR_ADD_I64, left, one);
  if (side_effect)
    nyir_emit(f, (nyir_inst_t){.op = NYIR_STORE_LOCAL, .dst = -1,
                               .a = false_value, .b = -1, .c = -1, .d = -1,
                               .e = -1, .f = -1, .imm = 0});
  ny_br(f, 30);
  ny_label(f, 20);
  int true_value = ny_binop(f, NYIR_ADD_I64, left, two);
  ny_br(f, 30);
  ny_label(f, 30);
  int result = f->next_value;
  ny_phi(f, 10, false_value, 20, true_value);
  ny_ret(f, result);
}

static size_t count_op(const nyir_func_t *f, nyir_op_t op) {
  size_t count = 0;
  for (size_t i = 0; i < f->len; ++i)
    count += f->data[i].op == op;
  return count;
}

static int run_case(const char *name, int64_t right, bool side_effect,
                    long long expected, bool converted) {
  nyir_func_t f;
  build_diamond(&f, right, side_effect);
  if (!ny_verify(&f, name))
    return 1;
  long long before = ny_eval(&f, name);
  if (!nyir_if_convert(&f) || !ny_verify(&f, name)) {
    nyir_func_free(&f);
    return 1;
  }
  long long after = ny_eval(&f, name);
  size_t selects = count_op(&f, NYIR_SELECT_I64);
  size_t phis = count_op(&f, NYIR_PHI);
  if ((converted && (selects != 1 || phis != 0)) ||
      (!converted && (selects != 0 || phis != 1))) {
    fprintf(stderr, "[%s] unexpected conversion select=%zu phi=%zu\n", name,
            selects, phis);
    nyir_func_free(&f);
    return 1;
  }
  nyir_func_free(&f);
  ny_check(name, before, after, expected);
  return 0;
}

int main(void) {
  return run_case("if-convert-false", 7, false, 5, true) ||
         run_case("if-convert-true", 4, false, 6, true) ||
         run_case("if-convert-side-effect", 7, true, 5, false);
}
