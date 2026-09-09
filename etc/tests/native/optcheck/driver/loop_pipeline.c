/*
 * Regression driver for bounded software pipelining (nyir_softpipe_rewrite).
 */
#include "util.h"

extern bool nyir_softpipe_rewrite(nyir_func_t *f);

/*
 * Counted reduction with a separable stage-A (depends only on iv) feeding the
 * carried accumulator:
 *   for iv in [0,trip): A = 3*iv + 1 ; acc += A
 *   result = 10 + sum_{iv=0}^{trip-1} (3*iv + 1)
 */
static void build_piped_reduction(nyir_func_t *f, int64_t trip) {
  ny_begin(f);
  int n = ny_const(f, trip);
  int zero = ny_const(f, 0);
  int one = ny_const(f, 1);
  int three = ny_const(f, 3);
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
  int t0 = ny_binop(f, NYIR_MUL_I64, iv, three);
  int t1 = ny_binop(f, NYIR_ADD_I64, t0, one);
  int next_acc = ny_binop(f, NYIR_ADD_I64, acc, t1);
  int next_iv = ny_binop(f, NYIR_ADD_I64, iv, one);
  ny_br(f, 10);
  f->data[iv_phi].phi_incoming[1].value = next_iv;
  f->data[acc_phi].phi_incoming[1].value = next_acc;
}

static bool run_case(int64_t trip, bool expect_pipelined) {
  nyir_func_t f;
  build_piped_reduction(&f, trip);
  if (!ny_verify(&f, "softpipe-before"))
    return false;
  long long expected = 10 + (3 * trip * (trip - 1)) / 2 + trip;
  long long before = ny_eval(&f, "softpipe-before");
  size_t len_before = f.len;

  if (!nyir_softpipe_rewrite(&f) || !ny_verify(&f, "softpipe-after")) {
    nyir_func_free(&f);
    return false;
  }
  long long after = ny_eval(&f, "softpipe-after");
  size_t len_after = f.len;
  bool pipelined = len_after != len_before;
  nyir_func_free(&f);

  if (before != after || after != expected) {
    fprintf(stderr,
            "[softpipe] trip=%lld before=%lld after=%lld expected=%lld\n",
            (long long)trip, (long long)before, (long long)after,
            (long long)expected);
    return false;
  }
  if (pipelined != expect_pipelined) {
    fprintf(stderr, "[softpipe] trip=%lld pipelined=%d expect=%d\n",
            (long long)trip, (int)pipelined, (int)expect_pipelined);
    return false;
  }
  printf("[softpipe] ok trip=%lld result=%lld %s\n", (long long)trip,
         (long long)after, pipelined ? "(pipelined)" : "(unchanged)");
  return true;
}

/*
 * The dead final prefetch would evaluate 100 / (trip - trip), so div/mod
 * stages must remain scalar even though their operands are otherwise affine.
 */
static bool run_non_speculatable_case(void) {
  nyir_func_t f;
  ny_begin(&f);
  int trip = ny_const(&f, 4);
  int zero = ny_const(&f, 0);
  int one = ny_const(&f, 1);
  int hundred = ny_const(&f, 100);
  int acc0 = ny_const(&f, 10);
  ny_br(&f, 10);

  ny_label(&f, 10);
  size_t iv_phi = f.len;
  int iv = f.next_value;
  ny_phi(&f, -1, zero, 11, zero);
  size_t acc_phi = f.len;
  int acc = f.next_value;
  ny_phi(&f, -1, acc0, 11, acc0);
  int cond = ny_cmp(&f, iv, trip, NYIR_CMP_LT);
  ny_br_if(&f, cond, 11);

  ny_label(&f, 20);
  ny_ret(&f, acc);

  ny_label(&f, 11);
  int denom = ny_binop(&f, NYIR_SUB_I64, trip, iv);
  int term = ny_binop(&f, NYIR_DIV_I64, hundred, denom);
  int next_acc = ny_binop(&f, NYIR_ADD_I64, acc, term);
  int next_iv = ny_binop(&f, NYIR_ADD_I64, iv, one);
  ny_br(&f, 10);
  f.data[iv_phi].phi_incoming[1].value = next_iv;
  f.data[acc_phi].phi_incoming[1].value = next_acc;

  if (!ny_verify(&f, "softpipe-div-before"))
    return false;
  long long before = ny_eval(&f, "softpipe-div-before");
  size_t len_before = f.len;
  bool ok = nyir_softpipe_rewrite(&f) &&
            ny_verify(&f, "softpipe-div-after");
  long long after = ok ? ny_eval(&f, "softpipe-div-after") : 0;
  size_t len_after = f.len;
  nyir_func_free(&f);

  if (!ok || before != 218 || after != before || len_after != len_before) {
    fprintf(stderr, "[softpipe] non-speculatable division was transformed\n");
    return false;
  }
  printf("[softpipe] ok non-speculatable division result=%lld (unchanged)\n",
         after);
  return true;
}


int main(void) {
  return run_case(4, true) && run_case(1, true) && run_case(8, true) &&
                 run_case(0, false) && run_non_speculatable_case()
             ? 0
             : 1;
}