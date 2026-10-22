/*
 * Loop-idiom rewrites must not retain a stale alias class on MUL -> SHL.
 */
#include "code/backend/ir/opt/loop.h"
#include "support.h"

static bool build_loop(nyr_func_t *f, size_t *mul_inst) {
  nyr_opt_begin(f);
  int limit = nyr_opt_const(f, 4);
  int zero = nyr_opt_const(f, 0);
  int one = nyr_opt_const(f, 1);
  int scale = nyr_opt_const(f, 8);
  nyr_opt_br(f, 10);
  nyr_opt_label(f, 10);
  size_t iv_phi = f->len;
  int iv = f->next_value;
  nyr_opt_phi(f, -1, zero, 11, zero);
  size_t acc_phi = f->len;
  int acc = f->next_value;
  nyr_opt_phi(f, -1, zero, 11, zero);
  int cond = nyr_opt_cmp(f, iv, limit, NYIR_CMP_LT);
  nyr_opt_br_if(f, cond, 11);
  nyr_opt_br(f, 12);
  nyr_opt_label(f, 11);
  *mul_inst = f->len;
  int product = nyr_opt_binop(f, NYIR_MUL_I64, iv, scale);
  int next_acc = nyr_opt_binop(f, NYIR_ADD_I64, acc, product);
  int next_iv = nyr_opt_binop(f, NYIR_ADD_I64, iv, one);
  nyr_opt_br(f, 10);
  f->data[iv_phi].phi_incoming[1].value = next_iv;
  f->data[acc_phi].phi_incoming[1].value = next_acc;
  nyr_opt_label(f, 12);
  nyr_opt_ret(f, acc);
  return nyr_opt_verify(f, "loop-idiom-metadata-before");
}

int main(void) {
  nyr_func_t f;
  size_t mul_inst = 0;
  if (!build_loop(&f, &mul_inst)) {
    nyr_ir_func_free(&f);
    return 1;
  }
  int64_t before = nyr_opt_eval(&f, "loop-idiom-metadata-before");
  int result = f.data[mul_inst].dst;
  f.data[mul_inst].alias_class = 77;
  f.data[mul_inst].scalar_class = NYIR_SCALAR_I64_RAW;
  f.data[mul_inst].semantic_ownership = 1;
  f.data[mul_inst].semantic_mutable = true;
  f.data[mul_inst].range =
      (nyr_range_t){.has_min = true, .has_max = true, .min = 0, .max = 24};
  f.data[mul_inst].debug =
      (nyr_debug_loc_t){.file = "<loop-idiom-test>", .line = 14, .column = 9};
  bool ok = nyr_loop_idiom(&f) &&
            nyr_opt_verify(&f, "loop-idiom-metadata-after") &&
            nyr_opt_eval(&f, "loop-idiom-metadata-after") == before;
  bool saw_result = false;
  for (size_t i = 0; ok && i < f.len; ++i) {
    const nyr_inst_t *in = &f.data[i];
    if (in->dst != result)
      continue;
    saw_result = true;
    ok = in->op == NYIR_SHL_I64 && in->alias_class == 0 &&
         in->scalar_class == NYIR_SCALAR_I64_RAW &&
         in->semantic_ownership == 1 && in->semantic_mutable &&
         in->range.has_min && in->range.has_max && in->range.min == 0 &&
         in->range.max == 24 && in->debug.line == 14 && in->debug.column == 9;
  }
  ok = ok && saw_result;
  if (!ok)
    nyr_dump(stderr, &f, "loop-idiom-metadata-after");
  nyr_ir_func_free(&f);
  if (ok)
    puts("[loop-idiom-metadata] alias invalidated; semantic facts preserved");
  return ok ? 0 : 1;
}
