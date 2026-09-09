/*
 * Regression driver for unroll-and-jam (nyir_unroll_jam).
 */
#include "util.h"

extern bool nyir_unroll_jam(nyir_func_t *f);

/*
 * Counted reduction loop: acc = 10 + sum_{i in [0,trip)} i.
 */
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

static bool run_case(int64_t trip, bool expect_jammed) {
  nyir_func_t f;
  build_counted_sum(&f, trip);
  if (!ny_verify(&f, "unroll-jam-before"))
    return false;
  /*
   * expected = 10 + trip*(trip-1)/2
   */
  long long expected = 10 + trip * (trip - 1) / 2;
  long long before = ny_eval(&f, "unroll-jam-before");
  size_t len_before = f.len;

  if (!nyir_unroll_jam(&f) || !ny_verify(&f, "unroll-jam-after")) {
    nyir_func_free(&f);
    return false;
  }
  long long after = ny_eval(&f, "unroll-jam-after");
  size_t len_after = f.len;
  bool jammed = len_after > len_before;
  nyir_func_free(&f);

  if (before != after || after != expected) {
    fprintf(stderr, "[unroll-jam] trip=%lld eval before=%lld after=%lld "
                    "expected=%lld\n",
            (long long)trip, (long long)before, (long long)after,
            (long long)expected);
    return false;
  }
  if (jammed != expect_jammed) {
    fprintf(stderr, "[unroll-jam] trip=%lld jammed=%d expect=%d\n",
            (long long)trip,
            (int)jammed, (int)expect_jammed);
    return false;
  }
  printf("[unroll-jam] ok trip=%lld result=%lld %s\n", (long long)trip,
         (long long)after, jammed ? "(jammed)" : "(unchanged)");
  return true;
}

int main(void) {
  return run_case(4, true) && run_case(2, true) && run_case(8, true) &&
                 run_case(3, false) && run_case(1, false) && run_case(0, false)
             ? 0
             : 1;
}