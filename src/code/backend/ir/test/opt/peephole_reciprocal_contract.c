/*
 * Reciprocal factoring is reassociation and requires explicit fast-math.
 */
#include "support.h"

static int emit_f64_local(nyr_func_t *f, int slot) {
  return nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                    .dst = -1,
                                    .a = -1,
                                    .b = -1,
                                    .c = -1,
                                    .d = -1,
                                    .e = -1,
                                    .f = -1,
                                    .imm = slot,
                                    .scalar_class = NYIR_SCALAR_F64});
}

static bool build(nyr_func_t *f, int *first_div, int *second_div) {
  static const char source[] = "peephole-reciprocal-origin.ny";
  nyr_opt_begin(f);
  int x = emit_f64_local(f, 0);
  int y = emit_f64_local(f, 1);
  int divisor = emit_f64_local(f, 2);
  *first_div = nyr_ir_emit(
      f, (nyr_inst_t){.op = NYIR_DIV_F64,
                       .dst = -1,
                       .a = x,
                       .b = divisor,
                       .c = -1,
                       .d = -1,
                       .e = -1,
                       .f = -1,
                       .scalar_class = NYIR_SCALAR_F64,
                       .debug = {.file = source, .line = 7, .column = 13}});
  *second_div = nyr_ir_emit(
      f, (nyr_inst_t){.op = NYIR_DIV_F64,
                       .dst = -1,
                       .a = y,
                       .b = divisor,
                       .c = -1,
                       .d = -1,
                       .e = -1,
                       .f = -1,
                       .scalar_class = NYIR_SCALAR_F64,
                       .debug = {.file = source, .line = 9, .column = 17}});
  f->data[f->len - 1].alias_class = 73;
  f->data[f->len - 1].range =
      (nyr_range_t){.has_min = true, .has_max = true, .min = -4, .max = 4};
  int sum = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_ADD_F64,
                                       .dst = -1,
                                       .a = *first_div,
                                       .b = *second_div,
                                       .c = -1,
                                       .d = -1,
                                       .e = -1,
                                       .f = -1,
                                       .scalar_class = NYIR_SCALAR_F64});
  nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_RET,
                             .dst = -1,
                             .a = sum,
                             .b = -1,
                             .c = -1,
                             .d = -1,
                             .e = -1,
                             .f = -1,
                             .scalar_class = NYIR_SCALAR_F64});
  return nyr_opt_verify(f, "peephole-reciprocal-before");
}

static const nyr_inst_t *find_def(const nyr_func_t *f, int value) {
  for (size_t i = 0; i < f->len; ++i)
    if (f->data[i].dst == value)
      return &f->data[i];
  return NULL;
}

static void count_ops(const nyr_func_t *f, int *divs, int *muls) {
  *divs = *muls = 0;
  for (size_t i = 0; i < f->len; ++i) {
    *divs += f->data[i].op == NYIR_DIV_F64;
    *muls += f->data[i].op == NYIR_MUL_F64;
  }
}

int main(void) {
  nyr_func_t strict;
  int strict_first, strict_second;
  if (!build(&strict, &strict_first, &strict_second)) {
    nyr_ir_func_free(&strict);
    return 1;
  }
  nyr_set_fast_math(false);
  bool ok = nyr_peephole(&strict) &&
            nyr_opt_verify(&strict, "peephole-reciprocal-strict");
  int divs = 0, muls = 0;
  count_ops(&strict, &divs, &muls);
  const nyr_inst_t *strict_second_def = find_def(&strict, strict_second);
  ok = ok && divs == 2 && muls == 0 && strict_first >= 0 && strict_second_def &&
       strict_second_def->op == NYIR_DIV_F64 &&
       strict_second_def->alias_class == 73;
  nyr_ir_func_free(&strict);

  nyr_func_t fast;
  int fast_first, fast_second;
  if (!build(&fast, &fast_first, &fast_second)) {
    nyr_ir_func_free(&fast);
    nyr_set_fast_math(false);
    return 1;
  }
  nyr_set_fast_math(true);
  ok = ok && nyr_peephole(&fast) &&
       nyr_opt_verify(&fast, "peephole-reciprocal-fast");
  count_ops(&fast, &divs, &muls);
  bool cleared_facts = false;
  bool reciprocal_metadata = false;
  for (size_t i = 0; i < fast.len; ++i)
    if (fast.data[i].dst == fast_second && fast.data[i].op == NYIR_MUL_F64)
      cleared_facts = fast.data[i].alias_class == 0 &&
                      !fast.data[i].range.has_min &&
                      !fast.data[i].range.has_max;
  for (size_t i = 0; i < fast.len; ++i) {
    const nyr_inst_t *in = &fast.data[i];
    if (in->op == NYIR_CONST_F64 && in->imm == INT64_C(4607182418800017408))
      reciprocal_metadata |= in->scalar_class == NYIR_SCALAR_F64 &&
                             in->debug.line == 7 && in->debug.column == 13;
    if (in->op == NYIR_DIV_F64)
      reciprocal_metadata &= in->scalar_class == NYIR_SCALAR_F64 &&
                             in->debug.line == 7 && in->debug.column == 13;
  }
  ok = ok && fast_first >= 0 && divs == 1 && muls == 2 && cleared_facts &&
       reciprocal_metadata;
  nyr_ir_func_free(&fast);
  nyr_set_fast_math(false);
  if (!ok)
    fprintf(stderr,
            "[peephole-reciprocal-contract] strict div/mul=2/0 fast="
            "%d/%d stale-derived-facts-cleared=%s generated-metadata=%s\n",
            divs, muls, cleared_facts ? "yes" : "no",
            reciprocal_metadata ? "yes" : "no");
  else
    puts("[peephole-reciprocal-contract] strict IEEE preserved; fast-math "
         "rewrite clears stale range and alias facts");
  return ok ? 0 : 1;
}
