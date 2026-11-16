/*
 * Constant folding preserves a local fact over an unconditional jump to its
 * unique immediately following label, but never carries it across a join.
 */
#include "opt.h"

static void store_local(nyr_func_t *f, int local, int value) {
  nyr_ir_emit(f,
              (nyr_inst_t){
                  .op = NYIR_STORE_LOCAL, .dst = -1, .a = value, .imm = local});
}

/* Helpers return value ids; locate the instruction that defines one. */
static const nyr_inst_t *def_of(const nyr_func_t *f, int value) {
  for (size_t i = 0; i < f->len; ++i)
    if (f->data[i].dst == value)
      return &f->data[i];
  return NULL;
}

static bool unique_predecessor_bridge(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int zero = nyr_opt_const(&f, 0);
  store_local(&f, 0, zero);
  nyr_opt_br(&f, 10);
  nyr_opt_label(&f, 10);
  int loaded = nyr_opt_emit_imm(&f, NYIR_LOAD_LOCAL, 0);
  nyr_opt_ret(&f, loaded);

  bool ok = nyr_const_fold(&f) &&
            nyr_opt_verify(&f, "const-fold-local-bridge") &&
            def_of(&f, loaded) && def_of(&f, loaded)->op == NYIR_CONST_I64 &&
            def_of(&f, loaded)->imm == 0;
  if (!ok)
    nyr_dump(stderr, &f, "const-fold-local-bridge");
  nyr_ir_func_free(&f);
  return ok;
}

static bool join_does_not_inherit_local_fact(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  f.param_count = 2;
  f.param_types = calloc(2, sizeof(*f.param_types));
  if (!f.param_types) {
    nyr_ir_func_free(&f);
    return false;
  }
  f.param_types[0] = NYIR_PARAM_I64;
  f.param_types[1] = NYIR_PARAM_I64;
  int condition = nyr_opt_emit_imm(&f, NYIR_LOAD_LOCAL, 1);
  nyr_opt_br_if(&f, condition, 20);
  int zero = nyr_opt_const(&f, 0);
  store_local(&f, 0, zero);
  nyr_opt_br(&f, 20);
  nyr_opt_label(&f, 20);
  int loaded = nyr_opt_emit_imm(&f, NYIR_LOAD_LOCAL, 0);
  nyr_opt_ret(&f, loaded);

  bool ok = nyr_const_fold(&f) && nyr_opt_verify(&f, "const-fold-local-join") &&
            def_of(&f, loaded) && def_of(&f, loaded)->op == NYIR_LOAD_LOCAL;
  int64_t inputs[][2] = {{7, 0}, {7, 1}, {-3, 0}, {-3, 1}};
  const int64_t expected[] = {0, 7, 0, -3};
  for (size_t i = 0; ok && i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
    int64_t result = 0;
    ok = nyr_opt_eval_locals(&f, "const-fold-local-join", inputs[i], 2,
                             &result) &&
         result == expected[i];
  }
  if (!ok)
    nyr_dump(stderr, &f, "const-fold-local-join");
  nyr_ir_func_free(&f);
  return ok;
}

int main(void) {
  bool ok = unique_predecessor_bridge() && join_does_not_inherit_local_fact();
  if (ok)
    puts("[const-fold-local-bridge] straight-line facts cross only unique "
         "jumps");
  return ok ? 0 : 1;
}
