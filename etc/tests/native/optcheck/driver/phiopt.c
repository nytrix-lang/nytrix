/*
 * Driver for nyir_phiopt: constant boolean PHI reduction only.
 *
 * The positive diamond folds its join PHI without changing CFG edges.  The
 * negative cases keep the PHI for an unknown condition and for a trapping arm.
 */
#include "util.h"

static void build_diamond(nyir_func_t *f, int mode) {
  ny_begin(f);
  int cond;
  if (mode == 0) {
    cond = ny_const(f, 1);
  } else {
    int a = ny_const(f, 0);
    int b = ny_const(f, 1);
    cond = ny_binop(f, NYIR_ADD_I64, a, b);
  }
  int no = ny_const(f, 0);
  int yes = ny_const(f, 1);
  ny_label(f, 10);
  ny_br_if(f, cond, 20);
  ny_label(f, 30);
  if (mode == 2) {
    int zero = ny_const(f, 0);
    int divisor = ny_binop(f, NYIR_ADD_I64, zero, zero);
    (void)ny_binop(f, NYIR_DIV_I64, yes, divisor);
  }
  ny_br(f, 40);
  ny_label(f, 20);
  ny_br(f, 40);
  ny_label(f, 40);
  ny_phi(f, 20, yes, 30, no);
  int result = f->next_value - 1;
  ny_ret(f, result);
}

static size_t phi_count(const nyir_func_t *f) {
  size_t count = 0;
  for (size_t i = 0; i < f->len; ++i)
    count += f->data[i].op == NYIR_PHI;
  return count;
}

int main(void) {
  nyir_func_t f;

  build_diamond(&f, 0);
  if (!ny_verify(&f, "phiopt/positive-before"))
    return 1;
  long long before = ny_eval(&f, "phiopt/positive-before");
  if (!nyir_phiopt(&f) || phi_count(&f) != 0 ||
      !ny_verify(&f, "phiopt/positive-after"))
    return 1;
  long long after = ny_eval(&f, "phiopt/positive-after");
  nyir_func_free(&f);
  ny_check("phiopt-positive", before, after, 1);

  build_diamond(&f, 1);
  if (!nyir_phiopt(&f) || phi_count(&f) != 1 ||
      !ny_verify(&f, "phiopt/unknown"))
    return 1;
  nyir_func_free(&f);

  build_diamond(&f, 2);
  if (!nyir_phiopt(&f) || phi_count(&f) != 1 ||
      !ny_verify(&f, "phiopt/trapping"))
    return 1;
  nyir_func_free(&f);
  return 0;
}
