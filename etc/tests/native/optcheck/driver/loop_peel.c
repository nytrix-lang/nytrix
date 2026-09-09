#include "util.h"

extern bool nyir_loop_peel(nyir_func_t *f);

static void build_counted_sum(nyir_func_t *f, int64_t trip) {
  ny_begin(f);
  int n = ny_const(f, trip);
  int zero = ny_const(f, 0);
  int one = ny_const(f, 1);
  int acc0 = ny_const(f, 10);
  ny_br(f, 10);

  ny_label(f, 10);
  size_t iv_phi = f->len;
  int iv = f->next_value;
  ny_phi(f, -1, zero, 11, zero);
  size_t acc_phi = f->len;
  int acc = f->next_value;
  ny_phi(f, -1, acc0, 11, acc0);
  int cond = ny_cmp(f, iv, n, NYIR_CMP_LT);
  ny_br_if(f, cond, 11);

  ny_label(f, 20);
  ny_ret(f, acc);

  ny_label(f, 11);
  int next_acc = ny_binop(f, NYIR_ADD_I64, acc, iv);
  int next_iv = ny_binop(f, NYIR_ADD_I64, iv, one);
  ny_br(f, 10);
  f->data[iv_phi].phi_incoming[1].value = next_iv;
  f->data[acc_phi].phi_incoming[1].value = next_acc;
}

static size_t count_labels(const nyir_func_t *f) {
  size_t n = 0;
  for (size_t i = 0; i < f->len; ++i)
    n += f->data[i].op == NYIR_LABEL;
  return n;
}

static bool run_positive(void) {
  nyir_func_t f;
  build_counted_sum(&f, 4);
  if (!ny_verify(&f, "loop-peel-before"))
    return false;
  long long before = ny_eval(&f, "loop-peel-before");
  size_t len_before = f.len, labels_before = count_labels(&f);
  if (!nyir_loop_peel(&f) || !ny_verify(&f, "loop-peel-after")) {
    nyir_func_free(&f);
    return false;
  }
  long long after = ny_eval(&f, "loop-peel-after");
  bool transformed = f.len > len_before && count_labels(&f) == labels_before + 1;
  ny_check("loop-peel", before, after, 16);
  if (!transformed)
    fprintf(stderr, "[loop-peel] expected one peeled block\n");
  nyir_func_free(&f);
  return transformed;
}

static bool run_zero_trip(void) {
  nyir_func_t f;
  build_counted_sum(&f, 0);
  size_t before_len = f.len;
  if (!nyir_loop_peel(&f) || !ny_verify(&f, "loop-peel-zero")) {
    nyir_func_free(&f);
    return false;
  }
  long long result = ny_eval(&f, "loop-peel-zero");
  bool unchanged = f.len == before_len && result == 10;
  if (!unchanged)
    fprintf(stderr, "[loop-peel-zero] zero-trip loop was peeled\n");
  nyir_func_free(&f);
  return unchanged;
}

int main(void) { return run_positive() && run_zero_trip() ? 0 : 1; }
