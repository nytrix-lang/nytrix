/*
 * Driver for nyr_phiopt: constant boolean PHI reduction only.
 *
 * The positive diamond folds its join PHI without changing CFG edges.  The
 * negative cases keep the PHI for an unknown condition and for a trapping arm.
 */
#include "support.h"

static void build_diamond(nyr_func_t *f, int mode) {
  nyr_opt_begin(f);
  int cond;
  if (mode == 0) {
    cond = nyr_opt_const(f, 1);
  } else {
    int a = nyr_opt_const(f, 0);
    int b = nyr_opt_const(f, 1);
    cond = nyr_opt_binop(f, NYIR_ADD_I64, a, b);
  }
  int no = nyr_opt_const(f, 0);
  int yes = nyr_opt_const(f, 1);
  nyr_opt_label(f, 10);
  nyr_opt_br_if(f, cond, 20);
  nyr_opt_label(f, 30);
  if (mode == 2) {
    int zero = nyr_opt_const(f, 0);
    int divisor = nyr_opt_binop(f, NYIR_ADD_I64, zero, zero);
    (void)nyr_opt_binop(f, NYIR_DIV_I64, yes, divisor);
  }
  nyr_opt_br(f, 40);
  nyr_opt_label(f, 20);
  nyr_opt_br(f, 40);
  nyr_opt_label(f, 40);
  nyr_opt_phi(f, 20, yes, 30, no);
  int result = f->next_value - 1;
  nyr_opt_ret(f, result);
}

static size_t phi_count(const nyr_func_t *f) {
  size_t count = 0;
  for (size_t i = 0; i < f->len; ++i)
    count += f->data[i].op == NYIR_PHI;
  return count;
}

int main(void) {
  nyr_func_t f;

  build_diamond(&f, 0);
  if (!nyr_opt_verify(&f, "phiopt/positive-before"))
    return 1;
  long long before = nyr_opt_eval(&f, "phiopt/positive-before");
  if (!nyr_phiopt(&f) || phi_count(&f) != 0 ||
      !nyr_opt_verify(&f, "phiopt/positive-after"))
    return 1;
  long long after = nyr_opt_eval(&f, "phiopt/positive-after");
  nyr_ir_func_free(&f);
  nyr_opt_check("phiopt-positive", before, after, 1);

  build_diamond(&f, 1);
  if (!nyr_phiopt(&f) || phi_count(&f) != 1 ||
      !nyr_opt_verify(&f, "phiopt/unknown"))
    return 1;
  nyr_ir_func_free(&f);

  build_diamond(&f, 2);
  if (!nyr_phiopt(&f) || phi_count(&f) != 1 ||
      !nyr_opt_verify(&f, "phiopt/trapping"))
    return 1;
  nyr_ir_func_free(&f);
  return 0;
}
