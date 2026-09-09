/*
 * Regression driver for the equality-saturation e-graph pass.
 */
#include "util.h"

extern bool nyir_egraph_rewrite(nyir_func_t *f);

/*
 *   v0 = x                (param-like leaf)
 *   v1 = mul(v0, 16)      -> shl(v0, 4)
 *   v2 = add(v1, 0)       -> v1 (saturates through v1's class)
 *   v3 = xor(v3, v3)      ?? no: build v3 = sub(v2, v2) -> 0
 *   v4 = add(v2, v2)      -> shl(v2, 1)
 *   v5 = mul(v4, 1)       -> v4
 *   v6 = and(v5, 0)      -> 0
 *   v7 = or(v5, 0)       -> v5
 *   v8 = div(v5, 1)       -> v5
 *   ret v6 + v2 + v8 + x...
 *
 * We check eval parity for x in a few values and that `mul by 16` is gone.
 */
static void build_fn(nyir_func_t *f, int64_t x) {
  ny_begin(f);
  /*
   * x as an "input": a local slot we set via eval's locals is complex;
   * instead use a const input so eval is deterministic per build.
   */
  int xv = ny_const(f, x);
  int m16 = ny_binop(f, NYIR_MUL_I64, xv, ny_const(f, 16));
  int a0 = ny_binop(f, NYIR_ADD_I64, m16, ny_const(f, 0));
  int s = ny_binop(f, NYIR_SUB_I64, a0, a0);
  int two = ny_binop(f, NYIR_ADD_I64, a0, a0);
  int mul1 = ny_binop(f, NYIR_MUL_I64, two, ny_const(f, 1));
  int and0 = ny_binop(f, NYIR_AND_I64, mul1, ny_const(f, 0));
  int or0 = ny_binop(f, NYIR_OR_I64, mul1, ny_const(f, 0));
  int div1 = ny_binop(f, NYIR_DIV_I64, mul1, ny_const(f, 1));
  /*
   * total = y + z + and + or + div + x
   */
  int acc = ny_binop(f, NYIR_ADD_I64, a0, s);
  acc = ny_binop(f, NYIR_ADD_I64, acc, and0);
  acc = ny_binop(f, NYIR_ADD_I64, acc, or0);
  acc = ny_binop(f, NYIR_ADD_I64, acc, div1);
  acc = ny_binop(f, NYIR_ADD_I64, acc, xv);
  ny_ret(f, acc);
}

static bool count_op(const nyir_func_t *f, nyir_op_t op, size_t *n) {
  *n = 0;
  for (size_t i = 0; i < f->len; ++i)
    *n += f->data[i].op == op;
  return true;
}

static bool run_case(int64_t x) {
  nyir_func_t f;
  build_fn(&f, x);
  if (!ny_verify(&f, "egraph-before"))
    return false;
  long long before = ny_eval(&f, "egraph-before");
  size_t mul_before = 0, shl_before = 0;
  count_op(&f, NYIR_MUL_I64, &mul_before);
  count_op(&f, NYIR_SHL_I64, &shl_before);

  if (!nyir_egraph_rewrite(&f) || !ny_verify(&f, "egraph-after")) {
    nyir_func_free(&f);
    return false;
  }
  long long after = ny_eval(&f, "egraph-after");
  size_t mul_after = 0, shl_after = 0;
  count_op(&f, NYIR_MUL_I64, &mul_after);
  count_op(&f, NYIR_SHL_I64, &shl_after);

  if (before != after) {
    fprintf(stderr, "[egraph] eval changed before=%lld after=%lld\n", before,
            after);
    nyir_func_free(&f);
    return false;
  }
  /*
   * The mul-by-16 should now be a shl-by-4 (e-graph strength reduction).
   */
  if (!(mul_after < mul_before && shl_after > shl_before)) {
    fprintf(stderr,
            "[egraph] strength reduction missing: mul %zu->%zu shl %zu->%zu\n",
            mul_before, mul_after, shl_before, shl_after);
    nyir_func_free(&f);
    return false;
  }
  printf("[egraph] ok result=%lld mul=%zu->%zu shl=%zu->%zu\n", after,
         mul_before, mul_after, shl_before, shl_after);
  nyir_func_free(&f);
  return true;
}

static void build_dominated_fn(nyir_func_t *f) {
  ny_begin(f);
  int x = ny_const(f, 41);
  ny_br(f, 1);
  ny_label(f, 1);
  int zero = ny_const(f, 0);
  int first = ny_binop(f, NYIR_ADD_I64, x, zero);
  ny_br(f, 2);
  ny_label(f, 2);
  int second = ny_binop(f, NYIR_ADD_I64, x, zero);
  ny_ret(f, second);
  (void)first;
}

static bool run_dominated_case(void) {
  nyir_func_t f;
  build_dominated_fn(&f);
  if (!ny_verify(&f, "egraph-dominated-before") ||
      !nyir_egraph_rewrite(&f) ||
      !ny_verify(&f, "egraph-dominated-after")) {
    return false;
  }
  bool reused = false;
  for (size_t i = 0; i < f.len; ++i) {
    if (f.data[i].op == NYIR_COPY && f.data[i].a == 2) {
      reused = true;
      break;
    }
  }
  if (!reused) {
    if (ny_want_dump())
      nyir_dump(stderr, &f, "egraph-dominated-after");
    fprintf(stderr, "[egraph] dominated materialization reuse missing\n");
    nyir_func_free(&f);
    return false;
  }
  printf("[egraph] dominated materialization reuse ok\n");
  nyir_func_free(&f);
  return true;
}

static bool run_random_case(uint64_t *state) {
  *state ^= *state << 13;
  *state ^= *state >> 7;
  *state ^= *state << 17;
  int64_t x = (int64_t)(*state % 2000001u) - 1000000;
  nyir_func_t before, after;
  build_fn(&before, x);
  build_fn(&after, x);
  if (!ny_verify(&before, "egraph-random-before") ||
      !ny_verify(&after, "egraph-random-input") ||
      ny_eval(&before, "egraph-random-before") !=
          ny_eval(&after, "egraph-random-input") ||
      !nyir_egraph_rewrite(&after) ||
      !ny_verify(&after, "egraph-random-after") ||
      ny_eval(&before, "egraph-random-before-final") !=
          ny_eval(&after, "egraph-random-after")) {
    fprintf(stderr, "[egraph] randomized equivalence failed x=%lld\n",
            (long long)x);
    nyir_func_free(&before);
    nyir_func_free(&after);
    return false;
  }
  nyir_func_free(&before);
  nyir_func_free(&after);
  return true;
}

static bool run_random_cases(void) {
  uint64_t state = UINT64_C(0x9e3779b97f4a7c15);
  for (size_t i = 0; i < 256; ++i)
    if (!run_random_case(&state))
      return false;
  printf("[egraph] randomized equivalence ok cases=256\n");
  return true;
}

int main(void) {
  return run_case(3) && run_case(1000) && run_case(-7) &&
                 run_dominated_case() && run_random_cases()
             ? 0
             : 1;
}