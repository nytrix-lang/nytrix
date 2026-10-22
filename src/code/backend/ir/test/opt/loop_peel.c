#include "support.h"

extern bool nyr_loop_peel(nyr_func_t *f);

static void build_counted_sum(nyr_func_t *f, int64_t trip) {
  nyr_opt_begin(f);
  int n = nyr_opt_const(f, trip);
  int zero = nyr_opt_const(f, 0);
  int one = nyr_opt_const(f, 1);
  int acc0 = nyr_opt_const(f, 10);
  nyr_opt_br(f, 10);

  nyr_opt_label(f, 10);
  size_t iv_phi = f->len;
  int iv = f->next_value;
  nyr_opt_phi(f, -1, zero, 11, zero);
  size_t acc_phi = f->len;
  int acc = f->next_value;
  nyr_opt_phi(f, -1, acc0, 11, acc0);
  int cond = nyr_opt_cmp(f, iv, n, NYIR_CMP_LT);
  nyr_opt_br_if(f, cond, 11);

  nyr_opt_label(f, 20);
  nyr_opt_ret(f, acc);

  nyr_opt_label(f, 11);
  int next_acc = nyr_opt_binop(f, NYIR_ADD_I64, acc, iv);
  int next_iv = nyr_opt_binop(f, NYIR_ADD_I64, iv, one);
  nyr_opt_br(f, 10);
  f->data[iv_phi].phi_incoming[1].value = next_iv;
  f->data[acc_phi].phi_incoming[1].value = next_acc;
}

static size_t count_labels(const nyr_func_t *f) {
  size_t n = 0;
  for (size_t i = 0; i < f->len; ++i)
    n += f->data[i].op == NYIR_LABEL;
  return n;
}

static bool run_positive(void) {
  nyr_func_t f;
  build_counted_sum(&f, 4);
  if (!nyr_opt_verify(&f, "loop-peel-before"))
    return false;
  long long before = nyr_opt_eval(&f, "loop-peel-before");
  size_t len_before = f.len, labels_before = count_labels(&f);
  if (!nyr_loop_peel(&f) || !nyr_opt_verify(&f, "loop-peel-after")) {
    nyr_ir_func_free(&f);
    return false;
  }
  long long after = nyr_opt_eval(&f, "loop-peel-after");
  bool transformed =
      f.len > len_before && count_labels(&f) == labels_before + 1;
  nyr_opt_check("loop-peel", before, after, 16);
  if (!transformed)
    fprintf(stderr, "[loop-peel] expected one peeled block\n");
  nyr_ir_func_free(&f);
  return transformed;
}

static bool run_zero_trip(void) {
  nyr_func_t f;
  build_counted_sum(&f, 0);
  size_t before_len = f.len;
  if (!nyr_loop_peel(&f) || !nyr_opt_verify(&f, "loop-peel-zero")) {
    nyr_ir_func_free(&f);
    return false;
  }
  long long result = nyr_opt_eval(&f, "loop-peel-zero");
  bool unchanged = f.len == before_len && result == 10;
  if (!unchanged)
    fprintf(stderr, "[loop-peel-zero] zero-trip loop was peeled\n");
  nyr_ir_func_free(&f);
  return unchanged;
}

int main(void) { return run_positive() && run_zero_trip() ? 0 : 1; }
