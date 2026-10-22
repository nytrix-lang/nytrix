/*
 * Hoisting an invariant division through its explicit zero-divisor guard
 * preserves the guard's exit, remaps the split preheader's PHI edge, and must
 * not speculate signed overflow.
 */
#include "support.h"

static bool build_loop(nyr_func_t *f, int64_t numerator_value,
                       int *out_is_zero) {
  nyr_opt_begin(f);
  nyr_opt_label(f, 5);

  int numerator = nyr_opt_const(f, numerator_value);
  int zero = nyr_opt_const(f, 0);
  int one = nyr_opt_const(f, 1);
  int divisor =
      nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_LOAD_LOCAL, .dst = -1, .imm = 0});
  int is_zero = nyr_opt_cmp(f, divisor, zero, NYIR_CMP_EQ);
  if (numerator < 0 || zero < 0 || one < 0 || divisor < 0 || is_zero < 0)
    return false;
  if (out_is_zero)
    *out_is_zero = is_zero;

  nyr_opt_br(f, 10);
  nyr_opt_label(f, 10);
  int iv = f->next_value;
  size_t iv_phi_at = f->len;
  nyr_opt_phi(f, 5, zero, 20, -1);
  int acc = f->next_value;
  size_t acc_phi_at = f->len;
  nyr_opt_phi(f, 5, zero, 20, -1);
  nyr_opt_br_if(f, is_zero, 90);
  nyr_opt_br(f, 11);

  nyr_opt_label(f, 11);
  int bound = nyr_opt_binop(f, NYIR_DIV_I64, numerator, divisor);
  int keep_going = nyr_opt_cmp(f, iv, bound, NYIR_CMP_LT);
  nyr_opt_br_if(f, keep_going, 20);
  nyr_opt_br(f, 60);

  nyr_opt_label(f, 20);
  int next_acc = nyr_opt_binop(f, NYIR_ADD_I64, acc, iv);
  int next_iv = nyr_opt_binop(f, NYIR_ADD_I64, iv, one);
  nyr_opt_br(f, 10);

  nyr_opt_label(f, 60);
  nyr_opt_ret(f, acc);
  nyr_opt_label(f, 90);
  int error_result = nyr_opt_const(f, -99);
  nyr_opt_ret(f, error_result);

  f->data[iv_phi_at].phi_incoming[1].value = next_iv;
  f->data[acc_phi_at].phi_incoming[1].value = next_acc;
  return nyr_alias_analysis(f) &&
         nyr_opt_verify(f, "licm-guarded-division-before");
}

static size_t first_op(const nyr_func_t *f, nyr_op_t op) {
  for (size_t i = 0; i < f->len; ++i)
    if (f->data[i].op == op)
      return i;
  return SIZE_MAX;
}

static size_t first_guard(const nyr_func_t *f, int condition,
                          size_t *out_count) {
  size_t first = SIZE_MAX;
  *out_count = 0;
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->op != NYIR_BR_IF || in->a != condition)
      continue;
    if (first == SIZE_MAX)
      first = i;
    ++*out_count;
  }
  return first;
}

static size_t first_label(const nyr_func_t *f, int64_t label) {
  for (size_t i = 0; i < f->len; ++i)
    if (f->data[i].op == NYIR_LABEL && f->data[i].imm == label)
      return i;
  return SIZE_MAX;
}

static bool same_outcome(nyr_func_t *f, int64_t divisor, bool expect_success,
                         int64_t expected_value) {
  int64_t locals[1] = {divisor};
  int64_t result = 0;
  char err[256] = {0};
  bool ok = nyr_opt_eval_outcome(f, locals, 1, 500, &result, err, sizeof(err));
  return ok == expect_success && (!expect_success || result == expected_value);
}

static bool run_case(int64_t numerator, int64_t divisor, bool should_hoist,
                     bool expect_success, int64_t expected_value) {
  nyr_func_t f;
  int is_zero = -1;
  if (!build_loop(&f, numerator, &is_zero)) {
    nyr_ir_func_free(&f);
    return false;
  }
  bool before_ok = same_outcome(&f, divisor, expect_success, expected_value);
  bool optimized =
      nyr_licm(&f) && nyr_opt_verify(&f, "licm-guarded-division-after");
  bool after_ok =
      optimized && same_outcome(&f, divisor, expect_success, expected_value);
  size_t division = first_op(&f, NYIR_DIV_I64);
  size_t header = first_label(&f, 10);
  size_t guard_count = 0;
  size_t guard = first_guard(&f, is_zero, &guard_count);
  bool placement_ok = division != SIZE_MAX && header != SIZE_MAX &&
                      (should_hoist ? division < header : division > header);
  bool guard_ok = guard_count == 1 && guard != SIZE_MAX &&
                  (should_hoist ? guard < header : guard > header);
  if (!before_ok || !after_ok || !placement_ok || !guard_ok) {
    fprintf(stderr,
            "[licm-guarded-division] numerator=%lld divisor=%lld "
            "hoist=%s before=%s after=%s div=%zu guard=%zu/%zu "
            "header=%zu\n",
            (long long)numerator, (long long)divisor,
            should_hoist ? "yes" : "no", before_ok ? "ok" : "failed",
            after_ok ? "ok" : "failed", division, guard, guard_count, header);
    nyr_dump(stderr, &f, "licm-guarded-division");
  }
  nyr_ir_func_free(&f);
  return before_ok && after_ok && placement_ok && guard_ok;
}

int main(void) {
  bool ok = run_case(10, 2, true, true, 10) &&
            run_case(10, 0, true, true, -99) &&
            run_case(INT64_MIN, -1, false, false, 0);
  if (ok)
    puts("[licm-guarded-division] ok guard, result, and overflow");
  return ok ? 0 : 1;
}
