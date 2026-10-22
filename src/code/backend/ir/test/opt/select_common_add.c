/*
 * Shared-addend factoring must preserve both SELECT outcomes and wrapping
 * i64 behavior while eliminating one mutually exclusive arm addition.
 */
#include "support.h"
#include <stdint.h>

static size_t count_op(const nyr_func_t *f, nyr_op_t op) {
  size_t count = 0;
  for (size_t i = 0; i < f->len; ++i)
    count += f->data[i].op == op;
  return count;
}

static const nyr_inst_t *find_def(const nyr_func_t *f, int value) {
  for (size_t i = 0; i < f->len; ++i)
    if (f->data[i].dst == value)
      return &f->data[i];
  return NULL;
}

static void set_class(nyr_func_t *f, int value, nyr_scalar_class_t cls) {
  for (size_t i = 0; i < f->len; ++i)
    if (f->data[i].dst == value)
      f->data[i].scalar_class = cls;
}

static int run_case(const char *name, int64_t right, int64_t expected,
                    nyr_scalar_class_t scalar_class,
                    nyr_scalar_class_t select_class, bool should_factor) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int left = nyr_opt_const(&f, 4);
  int compare = nyr_opt_const(&f, right);
  int common = nyr_opt_const(&f, 1);
  int condition = nyr_opt_cmp(&f, left, compare, NYIR_CMP_EQ);
  nyr_opt_br_if(&f, condition, 20);

  nyr_opt_label(&f, 10);
  int no_base = nyr_opt_const(&f, INT64_MIN);
  int no_value = nyr_opt_binop(&f, NYIR_ADD_I64, no_base, common);
  set_class(&f, no_value, scalar_class);
  nyr_opt_br(&f, 30);

  nyr_opt_label(&f, 20);
  int yes_base = nyr_opt_const(&f, INT64_MAX);
  int yes_value = nyr_opt_binop(&f, NYIR_ADD_I64, yes_base, common);
  set_class(&f, yes_value, scalar_class);
  nyr_opt_br(&f, 30);

  nyr_opt_label(&f, 30);
  int result = f.next_value;
  nyr_opt_phi(&f, 10, no_value, 20, yes_value);
  f.data[f.len - 1].scalar_class = select_class;
  f.data[f.len - 1].semantic_ownership = NY_SEM_OWN_SHARED;
  f.data[f.len - 1].semantic_mutable = true;
  nyr_opt_ret(&f, result);

  bool ok = nyr_opt_verify(&f, name);
  long long before = ok ? nyr_opt_eval(&f, name) : 0;
  ok = ok && nyr_if_convert(&f) && nyr_opt_verify(&f, "select-add-converted");
  if (ok)
    ok = count_op(&f, NYIR_SELECT_I64) == 1 &&
         nyr_opt_eval(&f, "select-add-converted") == before;
  if (ok)
    ok = nyr_peephole(&f) && nyr_opt_verify(&f, "select-add-factored");

  const nyr_inst_t *result_def = ok ? find_def(&f, result) : NULL;
  const nyr_inst_t *sum_def = result_def && result_def->op == NYIR_COPY
                                   ? find_def(&f, result_def->a)
                                   : NULL;
  bool factored = sum_def && sum_def->op == NYIR_ADD_I64;
  const nyr_inst_t *selected = factored ? find_def(&f, sum_def->a) : NULL;
  if (should_factor) {
    bool shape_ok = factored && result_def->op == NYIR_COPY && selected &&
                    selected->op == NYIR_SELECT_I64 &&
                    selected->b == yes_base && selected->c == no_base &&
                    sum_def->b == common;
    bool metadata_ok = result_def &&
                       result_def->scalar_class == NYIR_SCALAR_I64_RAW &&
                       result_def->semantic_ownership == NY_SEM_OWN_SHARED &&
                       result_def->semantic_mutable && selected &&
                       selected->scalar_class == NYIR_SCALAR_I64_RAW &&
                       selected->semantic_ownership == NY_SEM_OWN_UNKNOWN &&
                       !selected->semantic_mutable;
    if (!shape_ok || !metadata_ok)
      fprintf(stderr,
              "[%s] shape=%d result-op=%d result-class=%u result-owner=%u "
              "result-mutable=%d sum-op=%d selected-op=%d selected-class=%u "
              "selected-owner=%u selected-mutable=%d common=%d\n",
              name, shape_ok, result_def ? (int)result_def->op : -1,
              result_def ? (unsigned)result_def->scalar_class : 0,
              result_def ? (unsigned)result_def->semantic_ownership : 0,
              result_def ? result_def->semantic_mutable : 0,
              sum_def ? (int)sum_def->op : -1,
              selected ? (int)selected->op : -1,
              selected ? (unsigned)selected->scalar_class : 0,
              selected ? (unsigned)selected->semantic_ownership : 0,
              selected ? selected->semantic_mutable : 0, common);
    ok = ok && shape_ok && metadata_ok;
  } else
    ok = ok && !factored && result_def && result_def->op == NYIR_SELECT_I64 &&
         result_def->scalar_class == scalar_class &&
         result_def->semantic_ownership == NY_SEM_OWN_SHARED &&
         result_def->semantic_mutable && count_op(&f, NYIR_ADD_I64) == 2;

  if (should_factor && ok)
    ok = nyr_dce(&f) && nyr_opt_verify(&f, "select-add-dead-arms-removed") &&
         count_op(&f, NYIR_ADD_I64) == 1;
  long long after = ok ? nyr_opt_eval(&f, name) : 0;
  if (!ok) {
    fprintf(stderr, "[%s] shared-addend SELECT factoring failed\n", name);
    nyr_dump(stderr, &f, name);
  } else {
    nyr_opt_check(name, before, after, expected);
  }
  nyr_ir_func_free(&f);
  return ok && before == expected && after == expected ? 0 : 1;
}

int main(void) {
  return run_case("select-common-add-true-wrap", 4, INT64_MIN,
                  NYIR_SCALAR_I64_RAW, NYIR_SCALAR_I64_RAW, true) ||
         run_case("select-common-add-false-wrap", 7, INT64_MIN + 1,
                  NYIR_SCALAR_I64_RAW, NYIR_SCALAR_UNKNOWN, true) ||
         run_case("select-common-add-tagged-guard", 4, INT64_MIN,
                  NYIR_SCALAR_I64_TAGGED, NYIR_SCALAR_I64_TAGGED, false);
}
