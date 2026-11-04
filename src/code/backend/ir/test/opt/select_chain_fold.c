/*
 * The if-converted `dce` chain x' = select(x == i, x + 1, x + 2) collapses
 * to one select of two closed forms once x0 is known nonnegative. With an
 * unbounded x0 every step must stay, because x0 + 2i == i is reachable for
 * negative inputs.
 */
#include "support.h"
#include <stdint.h>

enum { STEPS = 24 };

static int64_t reference(int64_t x) {
  for (int64_t i = 0; i < STEPS; ++i)
    x = x == i ? x + 1 : x + 2;
  return x;
}

static void build(nyr_func_t *f, bool bounded) {
  nyr_opt_begin(f);
  int x = nyr_opt_emit_imm(f, NYIR_LOAD_LOCAL, 0);
  if (bounded)
    x = nyr_opt_binop(f, NYIR_AND_I64, x, nyr_opt_const(f, 0x7fff));
  for (int64_t i = 0; i < STEPS; ++i) {
    int c = nyr_opt_cmp(f, x, nyr_opt_const(f, i), NYIR_CMP_EQ);
    int t = nyr_opt_binop(f, NYIR_ADD_I64, x, nyr_opt_const(f, 1));
    int e = nyr_opt_binop(f, NYIR_ADD_I64, x, nyr_opt_const(f, 2));
    x = nyr_opt_select(f, c, t, e);
  }
  nyr_opt_ret(f, x);
}

static size_t count_selects(const nyr_func_t *f) {
  size_t n = 0;
  for (size_t i = 0; i < f->len; ++i)
    n += f->data[i].op == NYIR_SELECT_I64;
  return n;
}

static bool run(nyr_func_t *f, bool bounded, const char *tag) {
  static const int64_t inputs[] = {
      0, 1, 2, 5, 23, 24, 100, 0x7fff, -1, -3, -24, INT64_MIN, INT64_MAX};
  for (size_t k = 0; k < sizeof(inputs) / sizeof(inputs[0]); ++k) {
    int64_t locals[4] = {inputs[k]};
    int64_t got = 0;
    if (!nyr_opt_eval_locals(f, tag, locals, 4, &got))
      return false;
    int64_t x = bounded ? (inputs[k] & 0x7fff) : inputs[k];
    if (got != reference(x)) {
      fprintf(stderr, "[select-chain-fold] %s x=%lld got=%lld want=%lld\n", tag,
              (long long)inputs[k], (long long)got, (long long)reference(x));
      return false;
    }
  }
  return true;
}

static bool check(bool bounded) {
  nyr_func_t f;
  build(&f, bounded);
  bool ok = nyr_opt_verify(&f, "select-chain-before") &&
            run(&f, bounded, "before") && nyr_select_chain_fold(&f) &&
            nyr_copy_prop(&f) && nyr_const_fold(&f) && nyr_dce(&f) &&
            nyr_opt_verify(&f, "select-chain-after") &&
            run(&f, bounded, "after");
  size_t selects = ok ? count_selects(&f) : 0;
  /*
   * Bounded: one live select remains. Unbounded: the chain must stay.
   */
  ok = ok && (bounded ? selects == 1 : selects == STEPS);
  if (!ok) {
    fprintf(stderr, "[select-chain-fold] %s chain kept %zu selects\n",
            bounded ? "bounded" : "unbounded", selects);
    nyr_dump(stderr, &f, "select-chain-fold");
  }
  nyr_ir_func_free(&f);
  return ok;
}

int main(void) {
  bool ok = check(true) && check(false);
  if (ok)
    puts("[select-chain-fold] chain collapses only with a nonnegative seed");
  return ok ? 0 : 1;
}
