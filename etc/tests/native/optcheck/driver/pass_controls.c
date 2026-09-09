/*
 * Regression driver for NYIR disable-pass controls.
 */
#include "util.h"

static bool pass_control_parses(const char *pass) {
  nyir_func_t f;
  ny_begin(&f);
  int value = ny_const(&f, 42);
  ny_ret(&f, value);
  if (!ny_verify(&f, pass)) {
    nyir_func_free(&f);
    return false;
  }

  nyir_set_pass_controls(pass, NULL);
  bool ok = nyir_optimize(&f, 0);
  nyir_set_pass_controls(NULL, NULL);
  if (!ok) {
    fprintf(stderr, "[pass-controls] rejected %s\n", pass);
    nyir_func_free(&f);
    return false;
  }
  if (!ny_verify(&f, pass)) {
    nyir_func_free(&f);
    return false;
  }
  long long result = ny_eval(&f, pass);
  nyir_func_free(&f);
  if (result == 42)
    return true;
  fprintf(stderr, "[pass-controls] %s changed result to %lld\n", pass,
          result);
  return false;
}

int main(void) {
  static const char *const passes[] = {
      "adce", "affine_nest", "prefetch_insert", "polyhedral_tiling",
      "bounds_check_elim",
      "null_check_elim", "overflow_check_elim", "vrp", "cvp",
      "alias_analysis", "null_align_facts", "egraph_local", "iv_simplify",
      "narrow", "phi_elim", "memory_ssa_forward",
  };

  for (size_t i = 0; i < sizeof(passes) / sizeof(passes[0]); ++i)
    if (!pass_control_parses(passes[i]))
      return 1;
  return 0;
}
