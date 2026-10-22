/*
 * Closed-form reduction for an even counted loop alternating by IV parity.
 */
#include "support.h"

bool nyr_iv_elim(nyr_func_t *f);

static size_t count_op(const nyr_func_t *f, nyr_op_t op) {
  size_t n = 0;
  for (size_t i = 0; i < f->len; ++i)
    n += f->data[i].op == op;
  return n;
}

static bool build_periodic_sum(nyr_func_t *f, int64_t trips, int64_t step,
                               int64_t offset) {
  nyr_opt_begin(f);
  int trip_limit = nyr_opt_const(f, trips * step);
  int zero = nyr_opt_const(f, 0);
  int mask = nyr_opt_const(f, 1);
  int offset_value = nyr_opt_const(f, offset);
  int when_odd = nyr_opt_const(f, 100);
  int when_even = nyr_opt_const(f, 198);
  nyr_opt_br(f, 10);
  nyr_opt_label(f, 10);
  int iv = f->next_value;
  nyr_opt_phi(f, -1, zero, 11, zero);
  int accumulator = f->next_value;
  nyr_opt_phi(f, -1, zero, 11, zero);
  int keep_going = nyr_opt_cmp(f, iv, trip_limit, NYIR_CMP_LT);
  nyr_opt_br_if(f, keep_going, 11);
  nyr_opt_br(f, 12);
  nyr_opt_label(f, 11);
  int sum = nyr_opt_binop(f, NYIR_ADD_I64, iv, offset_value);
  int parity = nyr_opt_binop(f, NYIR_AND_I64, sum, mask);
  int is_odd = nyr_opt_cmp(f, parity, zero, NYIR_CMP_NE);
  int selected = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_SELECT_I64,
                                            .dst = -1,
                                            .a = is_odd,
                                            .b = when_odd,
                                            .c = when_even,
                                            .d = -1,
                                            .e = -1,
                                            .f = -1});
  int next_accumulator = nyr_opt_binop(f, NYIR_ADD_I64, accumulator, selected);
  int step_value = nyr_opt_const(f, step);
  int next_iv = nyr_opt_binop(f, NYIR_ADD_I64, iv, step_value);
  nyr_opt_br(f, 10);
  nyr_opt_label(f, 12);
  nyr_opt_ret(f, accumulator);

  /*
   * Patch both header PHIs to use their actual latch-produced SSA values.
   */
  for (size_t i = 0; i < f->len; ++i) {
    nyr_inst_t *in = &f->data[i];
    if (in->op != NYIR_PHI || in->phi_incoming_len != 2)
      continue;
    if (in->dst == iv)
      in->phi_incoming[1].value = next_iv;
    else if (in->dst == accumulator)
      in->phi_incoming[1].value = next_accumulator;
  }
  return nyr_opt_verify(f, "iv-period2/before");
}

static bool build_periodic_sum_with_early_exit(nyr_func_t *f) {
  nyr_opt_begin(f);
  int trip_limit = nyr_opt_const(f, 6);
  int zero = nyr_opt_const(f, 0);
  int mask = nyr_opt_const(f, 1);
  int stop_at = nyr_opt_const(f, 2);
  int when_odd = nyr_opt_const(f, 100);
  int when_even = nyr_opt_const(f, 198);
  nyr_opt_br(f, 10);
  nyr_opt_label(f, 10);
  int iv = f->next_value;
  nyr_opt_phi(f, -1, zero, 14, zero);
  int accumulator = f->next_value;
  nyr_opt_phi(f, -1, zero, 14, zero);
  int keep_going = nyr_opt_cmp(f, iv, trip_limit, NYIR_CMP_LT);
  nyr_opt_br_if(f, keep_going, 11);
  nyr_opt_br(f, 12);
  nyr_opt_label(f, 11);
  int should_stop = nyr_opt_cmp(f, iv, stop_at, NYIR_CMP_EQ);
  nyr_opt_br_if(f, should_stop, 13);
  nyr_opt_label(f, 14);
  int parity = nyr_opt_binop(f, NYIR_AND_I64, iv, mask);
  int is_odd = nyr_opt_cmp(f, parity, zero, NYIR_CMP_NE);
  int selected = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_SELECT_I64,
                                            .dst = -1,
                                            .a = is_odd,
                                            .b = when_odd,
                                            .c = when_even,
                                            .d = -1,
                                            .e = -1,
                                            .f = -1});
  int next_accumulator = nyr_opt_binop(f, NYIR_ADD_I64, accumulator, selected);
  int one = nyr_opt_const(f, 1);
  int next_iv = nyr_opt_binop(f, NYIR_ADD_I64, iv, one);
  nyr_opt_br(f, 10);
  nyr_opt_label(f, 12);
  nyr_opt_ret(f, accumulator);
  nyr_opt_label(f, 13);
  nyr_opt_ret(f, accumulator);

  for (size_t i = 0; i < f->len; ++i) {
    nyr_inst_t *in = &f->data[i];
    if (in->op != NYIR_PHI || in->phi_incoming_len != 2)
      continue;
    if (in->dst == iv)
      in->phi_incoming[1].value = next_iv;
    else if (in->dst == accumulator)
      in->phi_incoming[1].value = next_accumulator;
  }
  return nyr_opt_verify(f, "iv-period2/early-exit-before");
}

static bool run_case(const char *name, int64_t trips, int64_t step,
                     int64_t offset, int64_t expected, bool should_fold) {
  nyr_func_t f;
  if (!build_periodic_sum(&f, trips, step, offset)) {
    nyr_ir_func_free(&f);
    return false;
  }
  int64_t before = nyr_opt_eval(&f, name);
  bool ok = before == expected && nyr_iv_elim(&f) &&
            nyr_opt_verify(&f, name) && nyr_opt_eval(&f, name) == before;
  if (should_fold)
    ok = ok && count_op(&f, NYIR_BR_IF) == 0;
  else
    ok = ok && count_op(&f, NYIR_BR_IF) == 1;
  if (!ok) {
    fprintf(stderr, "[%s] unexpected periodic reduction result\n", name);
    nyr_dump(stderr, &f, name);
  } else {
    printf("[%s] ok result=%lld folded=%s\n", name, (long long)before,
           should_fold ? "yes" : "no");
  }
  nyr_ir_func_free(&f);
  return ok;
}

static bool run_early_exit_case(void) {
  nyr_func_t f;
  if (!build_periodic_sum_with_early_exit(&f)) {
    nyr_ir_func_free(&f);
    return false;
  }
  int64_t before = nyr_opt_eval(&f, "iv-period2/early-exit-before");
  bool ok = before == 298 && nyr_iv_elim(&f) &&
            nyr_opt_verify(&f, "iv-period2/early-exit-after") &&
            nyr_opt_eval(&f, "iv-period2/early-exit-after") == before &&
            count_op(&f, NYIR_BR_IF) == 2;
  if (!ok)
    fprintf(stderr,
            "[iv-period2/early-exit] closed form changed loop result\n");
  else
    printf("[iv-period2/early-exit] ok result=%lld folded=no\n",
           (long long)before);
  nyr_ir_func_free(&f);
  return ok;
}

int main(void) {
  return run_case("iv-period2/even-offset-even-trip", 6, 1, 0, 894, true) &&
                 run_case("iv-period2/odd-offset-even-trip", 6, 1, 1, 894,
                          true) &&
                 run_case("iv-period2/odd-trip-rejected", 5, 1, 0, 794,
                          false) &&
                 run_case("iv-period2/even-step-rejected", 6, 2, 1, 600,
                          false) &&
                 run_early_exit_case()
             ? 0
             : 1;
}
