/*
 * Regression driver for bounded software pipelining (nyr_softpipe_rewrite).
 */
#include "support.h"

extern bool nyr_softpipe_rewrite(nyr_func_t *f);

/*
 * Counted reduction with a separable stage-A (depends only on iv) feeding the
 * carried accumulator:
 *   for iv in [0,trip): A = 3*iv + 1 ; acc += A
 *   result = 10 + sum_{iv=0}^{trip-1} (3*iv + 1)
 */
static void build_piped_reduction(nyr_func_t *f, int64_t trip) {
  nyr_opt_begin(f);
  int n = nyr_opt_const(f, trip);
  int zero = nyr_opt_const(f, 0);
  int one = nyr_opt_const(f, 1);
  int three = nyr_opt_const(f, 3);
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
  int t0 = nyr_opt_binop(f, NYIR_MUL_I64, iv, three);
  int t1 = nyr_opt_binop(f, NYIR_ADD_I64, t0, one);
  int next_acc = nyr_opt_binop(f, NYIR_ADD_I64, acc, t1);
  int next_iv = nyr_opt_binop(f, NYIR_ADD_I64, iv, one);
  nyr_opt_br(f, 10);
  f->data[iv_phi].phi_incoming[1].value = next_iv;
  f->data[acc_phi].phi_incoming[1].value = next_acc;
}

static bool run_case(int64_t trip, bool expect_pipelined) {
  nyr_func_t f;
  build_piped_reduction(&f, trip);
  if (!nyr_opt_verify(&f, "softpipe-before"))
    return false;
  long long expected = 10 + (3 * trip * (trip - 1)) / 2 + trip;
  long long before = nyr_opt_eval(&f, "softpipe-before");
  size_t len_before = f.len;

  if (!nyr_softpipe_rewrite(&f) || !nyr_opt_verify(&f, "softpipe-after")) {
    nyr_ir_func_free(&f);
    return false;
  }
  long long after = nyr_opt_eval(&f, "softpipe-after");
  size_t len_after = f.len;
  bool pipelined = len_after != len_before;
  nyr_ir_func_free(&f);

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
  nyr_func_t f;
  nyr_opt_begin(&f);
  int trip = nyr_opt_const(&f, 4);
  int zero = nyr_opt_const(&f, 0);
  int one = nyr_opt_const(&f, 1);
  int hundred = nyr_opt_const(&f, 100);
  int acc0 = nyr_opt_const(&f, 10);
  nyr_opt_br(&f, 10);

  nyr_opt_label(&f, 10);
  size_t iv_phi = f.len;
  int iv = f.next_value;
  nyr_opt_phi(&f, -1, zero, 11, zero);
  size_t acc_phi = f.len;
  int acc = f.next_value;
  nyr_opt_phi(&f, -1, acc0, 11, acc0);
  int cond = nyr_opt_cmp(&f, iv, trip, NYIR_CMP_LT);
  nyr_opt_br_if(&f, cond, 11);

  nyr_opt_label(&f, 20);
  nyr_opt_ret(&f, acc);

  nyr_opt_label(&f, 11);
  int denom = nyr_opt_binop(&f, NYIR_SUB_I64, trip, iv);
  int term = nyr_opt_binop(&f, NYIR_DIV_I64, hundred, denom);
  int next_acc = nyr_opt_binop(&f, NYIR_ADD_I64, acc, term);
  int next_iv = nyr_opt_binop(&f, NYIR_ADD_I64, iv, one);
  nyr_opt_br(&f, 10);
  f.data[iv_phi].phi_incoming[1].value = next_iv;
  f.data[acc_phi].phi_incoming[1].value = next_acc;

  if (!nyr_opt_verify(&f, "softpipe-div-before"))
    return false;
  long long before = nyr_opt_eval(&f, "softpipe-div-before");
  size_t len_before = f.len;
  bool ok =
      nyr_softpipe_rewrite(&f) && nyr_opt_verify(&f, "softpipe-div-after");
  long long after = ok ? nyr_opt_eval(&f, "softpipe-div-after") : 0;
  size_t len_after = f.len;
  nyr_ir_func_free(&f);

  if (!ok || before != 218 || after != before || len_after != len_before) {
    fprintf(stderr, "[softpipe] non-speculatable division was transformed\n");
    return false;
  }
  printf("[softpipe] ok non-speculatable division result=%lld (unchanged)\n",
         after);
  return true;
}

/*
 * Soft-pipeline clones the stage-A arithmetic at three different dynamic
 * points. The look-ahead clone uses iv+1 and may execute once beyond the
 * original loop's last iteration, so a range proven only for the loop body
 * must not be copied onto the speculative result.
 */
static bool run_prefetch_range_invalidation_case(void) {
  nyr_func_t f;
  build_piped_reduction(&f, 4);
  int mul_dst = -1, add_dst = -1;
  for (size_t i = 0; i < f.len; ++i) {
    nyr_inst_t *in = &f.data[i];
    if (in->op == NYIR_MUL_I64 && mul_dst < 0) {
      mul_dst = in->dst;
      in->range =
          (nyr_range_t){.has_min = true, .has_max = true, .min = 0, .max = 9};
    } else if (in->op == NYIR_ADD_I64 && in->a == mul_dst && add_dst < 0) {
      add_dst = in->dst;
      in->range =
          (nyr_range_t){.has_min = true, .has_max = true, .min = 1, .max = 10};
    }
  }
  if (mul_dst < 0 || add_dst < 0 ||
      !nyr_opt_verify(&f, "softpipe-range-before")) {
    nyr_ir_func_free(&f);
    return false;
  }
  long long before = nyr_opt_eval(&f, "softpipe-range-before");
  bool ok =
      nyr_softpipe_rewrite(&f) && nyr_opt_verify(&f, "softpipe-range-after");
  long long after = ok ? nyr_opt_eval(&f, "softpipe-range-after") : 0;
  bool stale = false;
  for (size_t i = 0; i < f.len; ++i) {
    const nyr_inst_t *in = &f.data[i];
    if ((in->op == NYIR_MUL_I64 && in->range.has_min && in->range.has_max &&
         in->range.min == 0 && in->range.max == 9) ||
        (in->op == NYIR_ADD_I64 && in->range.has_min && in->range.has_max &&
         in->range.min == 1 && in->range.max == 10))
      stale = true;
  }
  nyr_ir_func_free(&f);
  if (!ok || before != after || stale) {
    fprintf(stderr,
            "[softpipe] cloned stage-A arithmetic retained stale loop range\n");
    return false;
  }
  printf("[softpipe] cloned stage-A range facts invalidated\n");
  return true;
}

int main(void) {
  return run_case(4, true) && run_case(1, true) && run_case(8, true) &&
                 run_case(0, false) && run_non_speculatable_case() &&
                 run_prefetch_range_invalidation_case()
             ? 0
             : 1;
}
