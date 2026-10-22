/*
 * Regression driver for the equality-saturation e-graph pass.
 */
#include "support.h"

extern bool nyr_egraph_rewrite(nyr_func_t *f);

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
static void build_fn(nyr_func_t *f, int64_t x) {
  nyr_opt_begin(f);
  /*
   * x as an "input": a local slot we set via eval's locals is complex;
   * instead use a const input so eval is deterministic per build.
   */
  int xv = nyr_opt_const(f, x);
  int m16 = nyr_opt_binop(f, NYIR_MUL_I64, xv, nyr_opt_const(f, 16));
  int a0 = nyr_opt_binop(f, NYIR_ADD_I64, m16, nyr_opt_const(f, 0));
  int s = nyr_opt_binop(f, NYIR_SUB_I64, a0, a0);
  int two = nyr_opt_binop(f, NYIR_ADD_I64, a0, a0);
  int mul1 = nyr_opt_binop(f, NYIR_MUL_I64, two, nyr_opt_const(f, 1));
  int and0 = nyr_opt_binop(f, NYIR_AND_I64, mul1, nyr_opt_const(f, 0));
  int or0 = nyr_opt_binop(f, NYIR_OR_I64, mul1, nyr_opt_const(f, 0));
  int div1 = nyr_opt_binop(f, NYIR_DIV_I64, mul1, nyr_opt_const(f, 1));
  /*
   * total = y + z + and + or + div + x
   */
  int acc = nyr_opt_binop(f, NYIR_ADD_I64, a0, s);
  acc = nyr_opt_binop(f, NYIR_ADD_I64, acc, and0);
  acc = nyr_opt_binop(f, NYIR_ADD_I64, acc, or0);
  acc = nyr_opt_binop(f, NYIR_ADD_I64, acc, div1);
  acc = nyr_opt_binop(f, NYIR_ADD_I64, acc, xv);
  nyr_opt_ret(f, acc);
}

static bool count_op(const nyr_func_t *f, nyr_op_t op, size_t *n) {
  *n = 0;
  for (size_t i = 0; i < f->len; ++i)
    *n += f->data[i].op == op;
  return true;
}

static bool run_case(int64_t x) {
  nyr_func_t f;
  build_fn(&f, x);
  if (!nyr_opt_verify(&f, "egraph-before"))
    return false;
  long long before = nyr_opt_eval(&f, "egraph-before");
  size_t mul_before = 0, shl_before = 0;
  count_op(&f, NYIR_MUL_I64, &mul_before);
  count_op(&f, NYIR_SHL_I64, &shl_before);

  if (!nyr_egraph_rewrite(&f) || !nyr_opt_verify(&f, "egraph-after")) {
    nyr_ir_func_free(&f);
    return false;
  }
  long long after = nyr_opt_eval(&f, "egraph-after");
  size_t mul_after = 0, shl_after = 0;
  count_op(&f, NYIR_MUL_I64, &mul_after);
  count_op(&f, NYIR_SHL_I64, &shl_after);

  if (before != after) {
    fprintf(stderr, "[egraph] eval changed before=%lld after=%lld\n", before,
            after);
    nyr_ir_func_free(&f);
    return false;
  }
  /*
   * The mul-by-16 should now be a shl-by-4 (e-graph strength reduction).
   */
  if (!(mul_after < mul_before && shl_after > shl_before)) {
    fprintf(stderr,
            "[egraph] strength reduction missing: mul %zu->%zu shl %zu->%zu\n",
            mul_before, mul_after, shl_before, shl_after);
    nyr_ir_func_free(&f);
    return false;
  }
  printf("[egraph] ok result=%lld mul=%zu->%zu shl=%zu->%zu\n", after,
         mul_before, mul_after, shl_before, shl_after);
  nyr_ir_func_free(&f);
  return true;
}

static void build_dominated_fn(nyr_func_t *f) {
  nyr_opt_begin(f);
  int x = nyr_opt_const(f, 41);
  nyr_opt_br(f, 1);
  nyr_opt_label(f, 1);
  int zero = nyr_opt_const(f, 0);
  int first = nyr_opt_binop(f, NYIR_ADD_I64, x, zero);
  nyr_opt_br(f, 2);
  nyr_opt_label(f, 2);
  int second = nyr_opt_binop(f, NYIR_ADD_I64, x, zero);
  nyr_opt_ret(f, second);
  (void)first;
}

static bool run_dominated_case(void) {
  nyr_func_t f;
  build_dominated_fn(&f);
  if (!nyr_opt_verify(&f, "egraph-dominated-before") ||
      !nyr_egraph_rewrite(&f) ||
      !nyr_opt_verify(&f, "egraph-dominated-after")) {
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
    if (nyr_opt_want_dump())
      nyr_dump(stderr, &f, "egraph-dominated-after");
    fprintf(stderr, "[egraph] dominated materialization reuse missing\n");
    nyr_ir_func_free(&f);
    return false;
  }
  printf("[egraph] dominated materialization reuse ok\n");
  nyr_ir_func_free(&f);
  return true;
}

static bool run_alias_invalidation_case(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int value = nyr_opt_const(&f, 37);
  int zero = nyr_opt_binop(&f, NYIR_SUB_I64, value, value);
  (void)nyr_opt_binop(&f, NYIR_ADD_I64, value, value);
  nyr_opt_ret(&f, zero);
  size_t def = f.len;
  for (size_t i = 0; i < f.len; ++i)
    if (f.data[i].dst == zero)
      def = i;
  if (def == f.len) {
    nyr_ir_func_free(&f);
    return false;
  }
  f.data[def].alias_class = 73;
  f.data[def].scalar_class = NYIR_SCALAR_I64_RAW;
  f.data[def].range =
      (nyr_range_t){.has_min = true, .has_max = true, .min = 0, .max = 74};
  int64_t before = nyr_opt_eval(&f, "egraph-alias-before");
  bool ok = nyr_opt_verify(&f, "egraph-alias-before") &&
            nyr_egraph_rewrite(&f) &&
            nyr_opt_verify(&f, "egraph-alias-after") &&
            nyr_opt_eval(&f, "egraph-alias-after") == before;
  bool saw_result = false;
  for (size_t i = 0; ok && i < f.len; ++i) {
    const nyr_inst_t *in = &f.data[i];
    if (in->dst != zero)
      continue;
    saw_result = true;
    ok = in->op == NYIR_CONST_I64 && in->imm == 0 && in->alias_class == 0 &&
         in->scalar_class == NYIR_SCALAR_I64_RAW && in->range.has_min &&
         in->range.has_max && in->range.min == 0 && in->range.max == 0;
  }
  ok = ok && saw_result;
  if (!ok)
    nyr_dump(stderr, &f, "egraph-alias-after");
  nyr_ir_func_free(&f);
  if (ok)
    puts("[egraph] constant extraction clears stale alias and range facts");
  return ok;
}

static uint64_t next_random(uint64_t *state) {
  *state ^= *state << 13;
  *state ^= *state >> 7;
  *state ^= *state << 17;
  return *state;
}

static bool build_random_fn(nyr_func_t *f, uint64_t seed, size_t steps) {
  int values[25];
  size_t count = 1;
  nyr_opt_begin(f);
  f->param_types = malloc(sizeof(*f->param_types));
  if (!f->param_types)
    return false;
  f->param_types[0] = NYIR_PARAM_I64;
  f->param_count = 1;
  values[0] = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                         .dst = -1,
                                         .a = -1,
                                         .b = -1,
                                         .c = -1,
                                         .d = -1,
                                         .e = -1,
                                         .f = -1,
                                         .imm = 0});
  if (values[0] < 0)
    return false;

  for (size_t i = 0; i < steps; ++i) {
    int lhs = values[next_random(&seed) % count];
    int rhs = values[next_random(&seed) % count];
    int result = -1;
    switch (next_random(&seed) % 8) {
    case 0:
      result = nyr_opt_binop(f, NYIR_ADD_I64, lhs, rhs);
      break;
    case 1:
      result = nyr_opt_binop(f, NYIR_SUB_I64, lhs, rhs);
      break;
    case 2: {
      static const int64_t factors[] = {0, 1, 2};
      int factor = nyr_opt_const(f, factors[next_random(&seed) % 3]);
      if (factor >= 0)
        result = nyr_opt_binop(f, NYIR_MUL_I64, lhs, factor);
      break;
    }
    case 3: {
      int mask = nyr_opt_const(f, -1);
      if (mask >= 0)
        result = nyr_opt_binop(f, NYIR_AND_I64, lhs, mask);
      break;
    }
    case 4: {
      int zero = nyr_opt_const(f, 0);
      if (zero >= 0)
        result = nyr_opt_binop(f, NYIR_OR_I64, lhs, zero);
      break;
    }
    case 5:
      result = nyr_opt_binop(f, NYIR_XOR_I64, lhs, rhs);
      break;
    case 6:
    case 7: {
      int shift = nyr_opt_const(f, (int64_t)(next_random(&seed) % 64u));
      nyr_op_t op = next_random(&seed) & 1u ? NYIR_SHL_I64 : NYIR_SAR_I64;
      if (shift >= 0)
        result = nyr_opt_binop(f, op, lhs, shift);
      break;
    }
    }
    if (result < 0)
      return false;
    values[count++] = result;
  }
  nyr_opt_ret(f, values[count - 1]);
  return true;
}

static bool eval_random_fn(nyr_func_t *f, int64_t input, int64_t *result) {
  int64_t locals[8] = {input};
  nyr_eval_result_t evaluated = {0};
  char err[256] = {0};
  if (!nyr_ir_eval(f, locals, 8, 500, &evaluated, err, sizeof(err))) {
    fprintf(stderr, "[egraph] randomized evaluation failed: %s\n", err);
    nyr_eval_result_free(&evaluated);
    return false;
  }
  *result = evaluated.result;
  nyr_eval_result_free(&evaluated);
  return true;
}

typedef bool (*counterexample_fn)(void *context, size_t steps, int64_t input,
                                  bool *fails);

static bool shrink_counterexample(counterexample_fn check, void *context,
                                  size_t *steps, int64_t *input) {
  bool changed;
  do {
    changed = false;
    for (size_t candidate = 0; candidate < *steps; ++candidate) {
      bool fails = false;
      if (!check(context, candidate, *input, &fails))
        return false;
      if (fails) {
        *steps = candidate;
        changed = true;
        break;
      }
    }
    if (changed)
      continue;

    static const int64_t simple_inputs[] = {0, 1, -1};
    for (size_t i = 0; i < sizeof(simple_inputs) / sizeof(simple_inputs[0]);
         ++i) {
      int64_t candidate = simple_inputs[i];
      bool fails = false;
      if (candidate == *input || *input == 0 ||
          (*input == 1 && candidate == -1))
        continue;
      if (!check(context, *steps, candidate, &fails))
        return false;
      if (fails) {
        *input = candidate;
        changed = true;
        break;
      }
    }
    if (changed)
      continue;

    for (int64_t candidate = *input / 2; candidate != 0; candidate /= 2) {
      bool fails = false;
      if (!check(context, *steps, candidate, &fails))
        return false;
      if (fails) {
        *input = candidate;
        changed = true;
        break;
      }
    }
  } while (changed);
  return true;
}

typedef struct {
  size_t min_steps;
  int64_t input;
} synthetic_failure_t;

static bool synthetic_failure(void *opaque, size_t steps, int64_t input,
                              bool *fails) {
  const synthetic_failure_t *context = opaque;
  *fails = steps >= context->min_steps && input == context->input;
  return true;
}

typedef struct {
  uint64_t seed;
  int64_t before;
  int64_t after;
} random_case_context_t;

static bool check_random_case(void *opaque, size_t steps, int64_t input,
                              bool *fails) {
  random_case_context_t *context = opaque;
  nyr_func_t before = {0}, after = {0};
  bool before_built = build_random_fn(&before, context->seed, steps);
  bool after_built =
      before_built && build_random_fn(&after, context->seed, steps);
  bool valid = before_built && after_built &&
               nyr_opt_verify(&before, "egraph-shrink-before") &&
               nyr_opt_verify(&after, "egraph-shrink-input") &&
               nyr_egraph_rewrite(&after) &&
               nyr_opt_verify(&after, "egraph-shrink-after");
  if (valid)
    valid = eval_random_fn(&before, input, &context->before) &&
            eval_random_fn(&after, input, &context->after);
  if (before_built)
    nyr_ir_func_free(&before);
  if (after_built)
    nyr_ir_func_free(&after);
  if (!valid)
    return false;
  *fails = context->before != context->after;
  return true;
}

static bool run_shrinker_case(void) {
  size_t steps = 24;
  int64_t input = 127;
  synthetic_failure_t simple_case = {.min_steps = 5, .input = -1};
  if (!shrink_counterexample(synthetic_failure, &simple_case, &steps, &input) ||
      steps != simple_case.min_steps || input != simple_case.input) {
    fprintf(stderr,
            "[egraph] deterministic shrink failed: steps=%zu input=%lld\n",
            steps, (long long)input);
    return false;
  }
  puts("[egraph] deterministic counterexample shrink ok steps=5 input=-1");

  steps = 24;
  input = 1024;
  synthetic_failure_t halving_case = {.min_steps = 3, .input = 8};
  if (!shrink_counterexample(synthetic_failure, &halving_case, &steps,
                             &input) ||
      steps != halving_case.min_steps || input != halving_case.input) {
    fprintf(stderr, "[egraph] halving shrink failed: steps=%zu input=%lld\n",
            steps, (long long)input);
    return false;
  }
  puts("[egraph] counterexample input halving ok steps=3 input=8");
  return true;
}

static bool run_random_case(uint64_t seed) {
  int64_t random_input = (int64_t)(next_random(&seed) % 2000001u) - 1000000;
  uint64_t formula_seed = seed;
  nyr_func_t before = {0}, after = {0};
  bool built = build_random_fn(&before, formula_seed, 24) &&
               build_random_fn(&after, formula_seed, 24);
  if (!built || !nyr_opt_verify(&before, "egraph-random-before") ||
      !nyr_opt_verify(&after, "egraph-random-input") ||
      !nyr_egraph_rewrite(&after) ||
      !nyr_opt_verify(&after, "egraph-random-after")) {
    fprintf(stderr,
            "[egraph] randomized construction/verification failed "
            "seed=%llu\n",
            (unsigned long long)formula_seed);
    if (nyr_opt_want_dump()) {
      nyr_dump(stderr, &before, "egraph-random-before");
      nyr_dump(stderr, &after, "egraph-random-after");
    }
    nyr_ir_func_free(&before);
    nyr_ir_func_free(&after);
    return false;
  }
  static const int64_t boundary_inputs[] = {INT64_MIN, -1000000, -1,       0,
                                            1,         1000000,  INT64_MAX};
  int64_t before_value = 0, after_value = 0, failing_input = random_input;
  bool evaluated = eval_random_fn(&before, random_input, &before_value) &&
                   eval_random_fn(&after, random_input, &after_value);
  bool equivalent = evaluated && before_value == after_value;
  for (size_t i = 0;
       equivalent && i < sizeof(boundary_inputs) / sizeof(boundary_inputs[0]);
       ++i) {
    failing_input = boundary_inputs[i];
    evaluated = eval_random_fn(&before, failing_input, &before_value) &&
                eval_random_fn(&after, failing_input, &after_value);
    equivalent = evaluated && before_value == after_value;
  }
  if (!evaluated) {
    fprintf(stderr,
            "[egraph] randomized evaluation failed seed=%llu input=%lld\n",
            (unsigned long long)formula_seed, (long long)failing_input);
    if (nyr_opt_want_dump()) {
      nyr_dump(stderr, &before, "egraph-random-before");
      nyr_dump(stderr, &after, "egraph-random-after");
    }
    nyr_ir_func_free(&before);
    nyr_ir_func_free(&after);
    return false;
  }
  if (!equivalent) {
    random_case_context_t context = {.seed = formula_seed};
    size_t minimized_steps = 24;
    int64_t minimized_input = failing_input;
    bool remains_failure = false;
    bool minimized =
        shrink_counterexample(check_random_case, &context, &minimized_steps,
                              &minimized_input) &&
        check_random_case(&context, minimized_steps, minimized_input,
                          &remains_failure) &&
        remains_failure;
    fprintf(stderr,
            "[egraph] randomized equivalence failed seed=%llu input=%lld "
            "before=%lld after=%lld minimized_steps=%zu "
            "minimized_input=%lld minimized_before=%lld minimized_after=%lld\n",
            (unsigned long long)formula_seed, (long long)failing_input,
            (long long)before_value, (long long)after_value, minimized_steps,
            (long long)minimized_input, (long long)context.before,
            (long long)context.after);
    if (!minimized)
      fprintf(stderr,
              "[egraph] counterexample shrinking could not be replayed\n");
    if (nyr_opt_want_dump()) {
      nyr_dump(stderr, &before, "egraph-random-before");
      nyr_dump(stderr, &after, "egraph-random-after");
    }
    nyr_ir_func_free(&before);
    nyr_ir_func_free(&after);
    return false;
  }
  nyr_ir_func_free(&before);
  nyr_ir_func_free(&after);
  return true;
}

static bool run_random_cases(void) {
  uint64_t state = UINT64_C(0x9e3779b97f4a7c15);
  for (size_t i = 0; i < 256; ++i)
    if (!run_random_case(next_random(&state)))
      return false;
  printf("[egraph] seeded randomized expression equivalence ok cases=256 "
         "depth=24\n");
  return true;
}

int main(void) {
  return run_case(3) && run_case(1000) && run_case(-7) &&
                 run_dominated_case() && run_alias_invalidation_case() &&
                 run_shrinker_case() && run_random_cases()
             ? 0
             : 1;
}
