#include "util.h"

static int call_known(nyir_func_t *f, const char *name, int arg) {
  int dst = f->next_value;
  nyir_inst_t in = {.op = NYIR_CALL, .dst = dst, .a = arg, .b = -1,
                    .c = -1, .d = -1, .e = -1, .f = -1, .imm = 1,
                    .symbol = name, .flags = NYIR_INST_F_EFFECTS_KNOWN,
                    .effects = NYIR_EFFECT_CALL};
  if (nyir_emit(f, in) != dst)
    return -1;
  f->data[f->len - 1].flags |= NYIR_INST_F_EFFECTS_KNOWN;
  f->data[f->len - 1].effects = NYIR_EFFECT_CALL;
  f->next_value = dst + 1;
  return dst;
}

static void build_callee(nyir_func_t *f) {
  ny_begin(f);
  f->param_count = 1;
  int x = nyir_emit(f, (nyir_inst_t){.op = NYIR_LOAD_LOCAL, .dst = -1,
                                     .a = -1, .b = -1, .c = -1, .d = -1,
                                     .e = -1, .f = -1, .imm = 0});
  int one = ny_const(f, 1);
  int sum = ny_binop(f, NYIR_ADD_I64, x, one);
  ny_ret(f, sum);
}

int main(void) {
  nyir_func_t caller, callee;
  ny_begin(&caller);
  int arg = ny_const(&caller, 41);
  int result = call_known(&caller, "pure_add_one", arg);
  ny_ret(&caller, result);
  build_callee(&callee);
  const nyir_inline_callee_t entries[] = {{"pure_add_one", &callee}};
  nyir_set_inline_callees(entries, 1);
  bool ok = ny_verify(&caller, "ipa-before") && nyir_ipa_constprop(&caller) &&
            ny_verify(&caller, "ipa-after") && caller.data[1].op == NYIR_CONST_I64 &&
            caller.data[1].imm == 42 && ny_eval(&caller, "ipa-eval") == 42;
  nyir_set_inline_callees(NULL, 0);
  nyir_func_free(&caller);
  nyir_func_free(&callee);
  if (!ok)
    fprintf(stderr, "[ipa-constprop] regression failed\n");
  return ok ? 0 : 1;
}
