/*
 * Copy propagation must keep call argument ownership facts aligned with the
 * producer after replacing a value-preserving alias.
 */
#include "support.h"

int main(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);

  int owned = nyr_ir_emit(
      &f, (nyr_inst_t){.op = NYIR_CALL,
                        .dst = -1,
                        .a = -1,
                        .b = -1,
                        .c = -1,
                        .d = -1,
                        .e = -1,
                        .f = -1,
                        .imm = 0,
                        .semantic_ownership = NY_SEM_OWN_OWNED,
                        .effects = NYIR_EFFECT_CALL | NYIR_EFFECT_ALLOCATION,
                        .symbol = "rt_list_new_raw"});
  int alias = nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_COPY,
                                          .dst = -1,
                                          .a = owned,
                                          .b = -1,
                                          .c = -1,
                                          .d = -1,
                                          .e = -1,
                                          .f = -1});
  size_t call_at = f.len;
  nyr_ir_emit(
      &f, (nyr_inst_t){.op = NYIR_CALL,
                        .dst = -1,
                        .a = alias,
                        .b = -1,
                        .c = -1,
                        .d = -1,
                        .e = -1,
                        .f = -1,
                        .imm = 1,
                        .effects = NYIR_EFFECT_CALL | NYIR_EFFECT_WRITE_MEMORY,
                        .symbol = "rt_list_as_tuple"});
  nyr_call_arg_ownership_set(&f.data[call_at], 0, NY_SEM_OWN_BORROWED);
  nyr_call_arg_contract_set(&f.data[call_at], 0, NYIR_ARG_CONTRACT_BORROWS);
  int result = nyr_opt_const(&f, 0);
  nyr_opt_ret(&f, result);

  bool ok = nyr_opt_verify(&f, "copy-prop-call-ownership/before") &&
            nyr_copy_prop(&f) &&
            nyr_opt_verify(&f, "copy-prop-call-ownership/after");
  if (ok) {
    const nyr_inst_t *call = &f.data[call_at];
    ok = call->op == NYIR_CALL && call->a == owned &&
         nyr_call_arg_ownership_get(call, 0) == NY_SEM_OWN_OWNED &&
         nyr_call_arg_contract_get(call, 0) == NYIR_ARG_CONTRACT_BORROWS;
  }
  if (!ok)
    fprintf(stderr,
            "[copy-prop-call-ownership] argument ownership was not refreshed "
            "with its producer\n");
  nyr_ir_func_free(&f);
  return ok ? 0 : 1;
}
