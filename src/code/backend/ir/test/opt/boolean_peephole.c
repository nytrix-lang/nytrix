/*
 * Canonical tagged boolean wrappers around comparisons should reduce to the
 * underlying predicate without changing its zero/one semantics.
 */
#include "support.h"
#include <stdint.h>

static int64_t bool_cell_left = 8;
static int64_t bool_cell_right = 5;

static int count_op(const nyr_func_t *f, nyr_op_t op) {
  int count = 0;
  for (size_t i = 0; i < f->len; ++i)
    count += f->data[i].op == op;
  return count;
}

static bool test_mod_parity(void) {
  const char *name = "mod-parity-compare-peephole";
  nyr_func_t f;
  nyr_opt_begin(&f);
  int address = nyr_opt_const(&f, (int64_t)(intptr_t)&bool_cell_left);
  int dividend = nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                             .dst = -1,
                                             .a = address,
                                             .b = -1,
                                             .c = -1,
                                             .d = -1,
                                             .e = -1,
                                             .f = -1});
  int two = nyr_opt_const(&f, 2);
  int zero = nyr_opt_const(&f, 0);
  int remainder = nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_MOD_I64,
                                              .dst = -1,
                                              .a = dividend,
                                              .b = two,
                                              .c = -1,
                                              .d = -1,
                                              .e = -1,
                                              .f = -1,
                                              .effects = NYIR_EFFECT_MAY_TRAP});
  for (size_t i = 0; i < f.len; ++i) {
    if (f.data[i].dst != remainder)
      continue;
    f.data[i].alias_class = 81;
    f.data[i].range =
        (nyr_range_t){.has_min = true, .has_max = true, .min = -1, .max = 1};
  }
  int copied = nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_COPY,
                                           .dst = -1,
                                           .a = remainder,
                                           .b = -1,
                                           .c = -1,
                                           .d = -1,
                                           .e = -1,
                                           .f = -1});
  int odd = nyr_opt_cmp(&f, copied, zero, NYIR_CMP_NE);
  nyr_opt_ret(&f, odd);
  if (!nyr_opt_verify(&f, name)) {
    nyr_ir_func_free(&f);
    return false;
  }

  const int64_t values[] = {INT64_MIN, -9, -2, -1, 0, 1, 2, 9, INT64_MAX};
  bool ok = true;
  for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
    bool_cell_left = values[i];
    int64_t before = nyr_opt_eval(&f, name);
    if (before != (values[i] % 2 != 0) || !nyr_peephole(&f) ||
        !nyr_opt_verify(&f, name) || count_op(&f, NYIR_MOD_I64) != 0 ||
        count_op(&f, NYIR_AND_I64) != 1 || nyr_opt_eval(&f, name) != before) {
      fprintf(stderr, "[%s] parity rewrite failed for %lld\n", name,
              (long long)values[i]);
      nyr_dump(stderr, &f, name);
      ok = false;
      break;
    }
    for (size_t k = 0; k < f.len; ++k) {
      const nyr_inst_t *in = &f.data[k];
      if (in->dst == remainder && in->op == NYIR_AND_I64)
        ok = in->alias_class == 0 && !in->range.has_min && !in->range.has_max;
    }
    if (!ok) {
      fprintf(stderr, "[%s] parity rewrite retained stale derived facts\n",
              name);
      nyr_ir_func_free(&f);
      bool_cell_left = 8;
      return false;
    }
  }
  nyr_ir_func_free(&f);
  bool_cell_left = 8;
  return ok;
}

static bool test_rotate_clears_alias(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int input = nyr_opt_const(&f, INT64_C(0x12345678));
  int right_count = nyr_opt_const(&f, 7);
  int left_count = nyr_opt_const(&f, 25);
  int shr = nyr_opt_binop(&f, NYIR_SAR_I64, input, right_count);
  int shl = nyr_opt_binop(&f, NYIR_SHL_I64, input, left_count);
  int joined = nyr_opt_binop(&f, NYIR_OR_I64, shr, shl);
  int mask = nyr_opt_const(&f, INT64_C(0xffffffff));
  int result = nyr_opt_binop(&f, NYIR_AND_I64, joined, mask);
  nyr_opt_ret(&f, result);
  size_t result_inst = f.len - 2;
  f.data[result_inst].alias_class = 55;
  f.data[result_inst].range = (nyr_range_t){
      .has_min = true, .has_max = true, .min = 0, .max = INT64_C(0xffffffff)};
  int64_t before = nyr_opt_eval(&f, "rotate-alias-before");
  bool ok = nyr_opt_verify(&f, "rotate-alias-before") && nyr_peephole(&f) &&
            nyr_opt_verify(&f, "rotate-alias-after") &&
            nyr_opt_eval(&f, "rotate-alias-after") == before;
  bool saw_result = false;
  for (size_t i = 0; ok && i < f.len; ++i) {
    const nyr_inst_t *in = &f.data[i];
    if (in->dst != result)
      continue;
    saw_result = true;
    ok = in->op == NYIR_ROR32_I64 && in->alias_class == 0 &&
         !in->range.has_min && !in->range.has_max;
  }
  ok = ok && saw_result;
  if (!ok)
    nyr_dump(stderr, &f, "rotate-alias-after");
  nyr_ir_func_free(&f);
  if (ok)
    puts("[peephole] rotate rewrite clears stale range and alias facts");
  return ok;
}

int main(void) {
  const char *name = "boolean-compare-peephole";
  nyr_func_t f;
  nyr_opt_begin(&f);
  int left_address = nyr_opt_const(&f, (int64_t)(intptr_t)&bool_cell_left);
  int right_address = nyr_opt_const(&f, (int64_t)(intptr_t)&bool_cell_right);
  int left = nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                         .dst = -1,
                                         .a = left_address,
                                         .b = -1,
                                         .c = -1,
                                         .d = -1,
                                         .e = -1,
                                         .f = -1});
  int right = nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                          .dst = -1,
                                          .a = right_address,
                                          .b = -1,
                                          .c = -1,
                                          .d = -1,
                                          .e = -1,
                                          .f = -1});
  int zero = nyr_opt_const(&f, 0);
  int one = nyr_opt_const(&f, 1);
  int comparison = nyr_opt_cmp(&f, left, right, NYIR_CMP_LT);
  int normalized = nyr_opt_cmp(&f, comparison, zero, NYIR_CMP_NE);
  int masked = nyr_opt_binop(&f, NYIR_AND_I64, normalized, one);
  int inverted = nyr_opt_cmp(&f, masked, zero, NYIR_CMP_EQ);
  nyr_opt_ret(&f, inverted);
  if (!nyr_opt_verify(&f, name)) {
    nyr_ir_func_free(&f);
    return 1;
  }
  long long before = nyr_opt_eval(&f, name);
  bool ok = nyr_peephole(&f) && nyr_opt_verify(&f, name) &&
            count_op(&f, NYIR_AND_I64) == 0;
  const nyr_inst_t *result_def = NULL;
  for (size_t i = 0; ok && i < f.len; ++i)
    if (f.data[i].dst == inverted)
      result_def = &f.data[i];
  ok = ok && result_def && result_def->op == NYIR_CMP_I64 &&
       result_def->a == left && result_def->b == right &&
       result_def->cmp == NYIR_CMP_GE;
  long long after = ok ? nyr_opt_eval(&f, name) : 0;
  if (!ok) {
    fprintf(stderr, "[%s] comparison boolean wrapper was not simplified\n",
            name);
    nyr_dump(stderr, &f, name);
  } else {
    nyr_opt_check(name, before, after, 1);
  }
  nyr_ir_func_free(&f);
  return ok && before == after && test_mod_parity() &&
                 test_rotate_clears_alias()
             ? 0
             : 1;
}
