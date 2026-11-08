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
    x = (int64_t)((uint64_t)x + (uint64_t)(x == i ? 1 : 2));
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

static bool check_delta(int64_t key, bool fold, int true_step,
                        int false_step) {
  static const int64_t inputs[] = {
      INT64_MIN, INT64_MIN + 1, -50, -25, -24, -10, -1,
      0, 1, 6, 7, 8, 24, 100, INT64_MAX - 1, INT64_MAX};
  nyr_func_t f;
  nyr_opt_begin(&f);
  int x = nyr_opt_emit_imm(&f, NYIR_LOAD_LOCAL, 0);
  for (int i = 0; i < STEPS; ++i) {
    int cmp = nyr_opt_cmp(&f, x, nyr_opt_const(&f, key + i * true_step), NYIR_CMP_EQ);
    int delta = nyr_opt_select(&f, cmp, nyr_opt_const(&f, true_step),
                               nyr_opt_const(&f, false_step));
    x = nyr_opt_binop(&f, NYIR_ADD_I64, x, delta);
  }
  nyr_opt_ret(&f, x);
  bool ok = nyr_opt_verify(&f, "delta-before") &&
            nyr_select_chain_fold(&f) &&
            nyr_copy_prop(&f) && nyr_const_fold(&f) && nyr_dce(&f) &&
            nyr_opt_verify(&f, "delta-after");
  size_t selects = ok ? count_selects(&f) : 0;
  if (fold && selects != 1)
    ok = false;
  if (!fold && selects <= 1)
    ok = false;
  uint64_t rng = UINT64_C(0x9e3779b97f4a7c15);
  for (size_t i = 0; i < 256 && ok; ++i) {
    rng ^= rng >> 12;
    rng ^= rng << 25;
    rng ^= rng >> 27;
    int64_t x = i < sizeof(inputs) / sizeof(inputs[0]) ? inputs[i] :
                (int64_t)(rng * UINT64_C(0x2545f4914f6cdd1d));
    int64_t locals[4] = {x}, got = 0;
    int64_t expected = x;
    for (int j = 0; j < STEPS; ++j)
      expected = (int64_t)((uint64_t)expected +
                    (uint64_t)(expected == key + j * true_step
                                  ? true_step : false_step));
    if (!nyr_opt_eval_locals(&f, "delta-after", locals, 4, &got) ||
        got != expected)
      ok = false;
  }
  if (!ok)
    fprintf(stderr, "[select-chain-fold] delta key=%lld true=%d false=%d selects=%zu\n",
            (long long)key, true_step, false_step, selects);
  nyr_ir_func_free(&f);
  return ok;
}

static bool check_multiple(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int roots[2] = {nyr_opt_emit_imm(&f, NYIR_LOAD_LOCAL, 0),
                  nyr_opt_emit_imm(&f, NYIR_LOAD_LOCAL, 1)};
  int steps[2] = {1, -2};
  int misses[2] = {3, -6};
  int keys[2] = {7, -11};
  int dst[2] = {roots[0], roots[1]};
  for (int chain = 0; chain < 2; ++chain) {
    for (int i = 0; i < STEPS; ++i) {
      int cmp = nyr_opt_cmp(&f, dst[chain],
                            nyr_opt_const(&f, keys[chain] + i * steps[chain]),
                            NYIR_CMP_EQ);
      int delta = nyr_opt_select(&f, cmp, nyr_opt_const(&f, steps[chain]),
                                 nyr_opt_const(&f, misses[chain]));
      dst[chain] = nyr_opt_binop(&f, NYIR_ADD_I64, dst[chain], delta);
    }
  }
  nyr_opt_ret(&f, nyr_opt_binop(&f, NYIR_ADD_I64, dst[0], dst[1]));
  bool ok = nyr_opt_verify(&f, "multi-before") &&
            nyr_select_chain_fold(&f) && nyr_copy_prop(&f) &&
            nyr_const_fold(&f) && nyr_dce(&f) &&
            nyr_opt_verify(&f, "multi-after") && count_selects(&f) == 2;
  uint64_t state = UINT64_C(0xc01dbeef12345678);
  for (int i = 0; i < 256 && ok; ++i) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    int64_t input[4] = {(int64_t)state, (int64_t)(state ^ (state >> 23))};
    if (i == 0) input[0] = INT64_MIN, input[1] = INT64_MAX;
    if (i == 1) input[0] = 7, input[1] = -11;
    if (i == 2) input[0] = 5, input[1] = -7;
    int64_t exp[2] = {input[0], input[1]};
    for (int j = 0; j < 2; ++j)
      for (int k = 0; k < STEPS; ++k) {
        int64_t key = keys[j] + k * steps[j];
        int64_t d = exp[j] == key ? steps[j] : misses[j];
        exp[j] = (int64_t)((uint64_t)exp[j] + (uint64_t)d);
      }
    int64_t got = 0;
    if (!nyr_opt_eval_locals(&f, "multi-after", input, 4, &got) ||
        got != (int64_t)((uint64_t)exp[0] + (uint64_t)exp[1]))
      ok = false;
  }
  if (!ok)
    fprintf(stderr, "[select-chain-fold] multiple chains failed\n");
  nyr_ir_func_free(&f);
  return ok;
}

static bool check_many(void) {
  enum { CHAIN_COUNT = 24 };
  nyr_func_t f;
  nyr_opt_begin(&f);
  int sum = -1;
  for (int c = 0; c < CHAIN_COUNT; ++c) {
    int x = nyr_opt_emit_imm(&f, NYIR_LOAD_LOCAL, c);
    int key = c * 3 - 30, step = c % 5 - 2;
    int gap = (c % 2 ? -1 : 1) * (c % 8 + 1);
    for (int j = 0; j < STEPS; ++j) {
      int cmp = nyr_opt_cmp(&f, x, nyr_opt_const(&f, key + j * step),
                            NYIR_CMP_EQ);
      int delta = nyr_opt_select(&f, cmp, nyr_opt_const(&f, step),
                                 nyr_opt_const(&f, step + gap));
      x = nyr_opt_binop(&f, NYIR_ADD_I64, x, delta);
    }
    sum = sum < 0 ? x : nyr_opt_binop(&f, NYIR_ADD_I64, sum, x);
  }
  nyr_opt_ret(&f, sum);
  bool ok = nyr_opt_verify(&f, "many-before") &&
            nyr_select_chain_fold(&f) && nyr_copy_prop(&f) &&
            nyr_const_fold(&f) && nyr_dce(&f) &&
            nyr_opt_verify(&f, "many-after") &&
            count_selects(&f) == CHAIN_COUNT;
  for (int t = 0; t < 128 && ok; ++t) {
    int64_t locals[CHAIN_COUNT];
    uint64_t expected = 0;
    for (int c = 0; c < CHAIN_COUNT; ++c) {
      int key = c * 3 - 30, step = c % 5 - 2;
      int gap = (c % 2 ? -1 : 1) * (c % 8 + 1);
      int64_t x = (int64_t)((uint64_t)(t * 193 + c * 17) ^
                            (t & 1 ? UINT64_C(0xffffffffffffffff) : 0));
      if (t == 0) x = key;
      if (t == 1) x = key - gap * (c % STEPS);
      locals[c] = x;
      for (int j = 0; j < STEPS; ++j) {
        int inc = x == key + j * step ? step : step + gap;
        x = (int64_t)((uint64_t)x + (uint64_t)inc);
      }
      expected += (uint64_t)x;
    }
    int64_t got = 0;
    if (!nyr_opt_eval_locals(&f, "many-after", locals, CHAIN_COUNT, &got) ||
        (uint64_t)got != expected)
      ok = false;
  }
  if (!ok)
    fprintf(stderr, "[select-chain-fold] 24 independent chains failed\n");
  nyr_ir_func_free(&f);
  return ok;
}

int main(void) {
  bool ok = check(true) && check(false);
  const int keys[] = {0, 7, -13};
  const int steps[] = {-16, -3, -1, 0, 1, 3, 16};
  const int gaps[] = {-8, -7, -6, -5, -4, -3, -2, -1,
                       1, 2, 3, 4, 5, 6, 7, 8};
  for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]) && ok; ++k)
    for (size_t a = 0; a < sizeof(steps) / sizeof(steps[0]) && ok; ++a)
      for (size_t g = 0; g < sizeof(gaps) / sizeof(gaps[0]) && ok; ++g)
        ok = check_delta(keys[k], true, steps[a], steps[a] + gaps[g]);
  ok = ok && check_delta(INT64_MAX - STEPS, false, 1, 2) &&
             check_delta(INT64_MIN + 1, false, 1, 2) &&
             check_delta(INT64_MAX - STEPS, false, 1, -3) &&
             check_multiple() && check_many();
  if (ok)
    puts("[select-chain-fold] 336 wrapping-delta cases, 24 independent chains verified");
  return ok ? 0 : 1;
}
