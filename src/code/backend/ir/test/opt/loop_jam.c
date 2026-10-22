/*
 * Regression driver for unroll-and-jam (nyr_unroll_jam).
 */
#include "support.h"

extern bool nyr_unroll_jam(nyr_func_t *f);

/*
 * Counted reduction loop: acc = 10 + sum_{i in [0,trip)} i.
 */
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

static bool run_case(int64_t trip, bool expect_jammed) {
  nyr_func_t f;
  build_counted_sum(&f, trip);
  if (!nyr_opt_verify(&f, "unroll-jam-before"))
    return false;
  /*
   * expected = 10 + trip*(trip-1)/2
   */
  long long expected = 10 + trip * (trip - 1) / 2;
  long long before = nyr_opt_eval(&f, "unroll-jam-before");
  size_t len_before = f.len;

  if (!nyr_unroll_jam(&f) || !nyr_opt_verify(&f, "unroll-jam-after")) {
    nyr_ir_func_free(&f);
    return false;
  }
  long long after = nyr_opt_eval(&f, "unroll-jam-after");
  size_t len_after = f.len;
  bool jammed = len_after > len_before;
  nyr_ir_func_free(&f);

  if (before != after || after != expected) {
    fprintf(stderr,
            "[unroll-jam] trip=%lld eval before=%lld after=%lld "
            "expected=%lld\n",
            (long long)trip, (long long)before, (long long)after,
            (long long)expected);
    return false;
  }
  if (jammed != expect_jammed) {
    fprintf(stderr, "[unroll-jam] trip=%lld jammed=%d expect=%d\n",
            (long long)trip, (int)jammed, (int)expect_jammed);
    return false;
  }
  printf("[unroll-jam] ok trip=%lld result=%lld %s\n", (long long)trip,
         (long long)after, jammed ? "(jammed)" : "(unchanged)");
  return true;
}

static void build_multi_carried_loop(nyr_func_t *f, int64_t trip) {
  nyr_opt_begin(f);
  int n = nyr_opt_const(f, trip);
  int zero = nyr_opt_const(f, 0);
  int one = nyr_opt_const(f, 1);
  int acc_init = nyr_opt_const(f, 10);
  int weighted_init = nyr_opt_const(f, 3);
  nyr_opt_br(f, 10);

  nyr_opt_label(f, 10);
  size_t iv_phi = f->len;
  int iv = f->next_value;
  nyr_opt_phi(f, -1, zero, 11, zero);
  size_t acc_phi = f->len;
  int acc = f->next_value;
  nyr_opt_phi(f, -1, acc_init, 11, acc_init);
  size_t weighted_phi = f->len;
  int weighted = f->next_value;
  nyr_opt_phi(f, -1, weighted_init, 11, weighted_init);
  int cond = nyr_opt_cmp(f, iv, n, NYIR_CMP_LT);
  nyr_opt_br_if(f, cond, 11);

  nyr_opt_label(f, 20);
  int result = nyr_opt_binop(f, NYIR_ADD_I64, acc, weighted);
  nyr_opt_ret(f, result);

  nyr_opt_label(f, 11);
  int next_acc = nyr_opt_binop(f, NYIR_ADD_I64, acc, iv);
  int twice_iv = nyr_opt_binop(f, NYIR_ADD_I64, iv, iv);
  int weighted_term = nyr_opt_binop(f, NYIR_ADD_I64, twice_iv, one);
  int next_weighted = nyr_opt_binop(f, NYIR_ADD_I64, weighted, weighted_term);
  int next_iv = nyr_opt_binop(f, NYIR_ADD_I64, iv, one);
  nyr_opt_br(f, 10);
  f->data[iv_phi].phi_incoming[1].value = next_iv;
  f->data[acc_phi].phi_incoming[1].value = next_acc;
  f->data[weighted_phi].phi_incoming[1].value = next_weighted;
}

static bool run_multi_carried_case(void) {
  nyr_func_t f;
  build_multi_carried_loop(&f, 4);
  if (!nyr_opt_verify(&f, "unroll-jam-multi-carried-before")) {
    nyr_ir_func_free(&f);
    return false;
  }
  long long before = nyr_opt_eval(&f, "unroll-jam-multi-carried-before");
  size_t len_before = f.len;
  bool transformed = nyr_unroll_jam(&f) &&
                     nyr_opt_verify(&f, "unroll-jam-multi-carried-after");
  long long after =
      transformed ? nyr_opt_eval(&f, "unroll-jam-multi-carried-after") : 0;
  bool ok =
      transformed && before == 35 && after == before && f.len > len_before;
  nyr_ir_func_free(&f);
  if (!ok) {
    fprintf(stderr,
            "[unroll-jam] multiple carried PHIs changed: "
            "transformed=%d before=%lld after=%lld expected=35\n",
            (int)transformed, before, after);
    return false;
  }
  puts("[unroll-jam] two carried recurrences remap through the cloned body");
  return true;
}

int main(void) {
  return run_case(4, true) && run_case(2, true) && run_case(8, true) &&
                 run_case(3, false) && run_case(1, false) &&
                 run_case(0, false) && run_multi_carried_case()
             ? 0
             : 1;
}