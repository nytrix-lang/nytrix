/*
 * The noalias call fact is accepted only when the call returns a borrow of
 * exactly one formal argument.
 */
#include "support.h"

static void emit_call(nyr_func_t *f, unsigned flags, bool returns_borrow) {
  int arg = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_CONST_I64,
                                       .dst = -1,
                                       .a = -1,
                                       .b = -1,
                                       .c = -1,
                                       .d = -1,
                                       .e = -1,
                                       .f = -1});
  int result =
      nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_CALL,
                                 .dst = -1,
                                 .a = arg,
                                 .b = -1,
                                 .c = -1,
                                 .d = -1,
                                 .e = -1,
                                 .f = -1,
                                 .imm = 1,
                                 .flags = flags,
                                 .semantic_ownership = NY_SEM_OWN_BORROWED,
                                 .symbol = "borrow_mut"});
  if (returns_borrow)
    nyr_call_arg_contract_set(&f->data[1], 0,
                               NYIR_ARG_CONTRACT_RETURNS_BORROW);
  nyr_opt_ret(f, result);
}

int main(void) {
  nyr_func_t f;
  char err[256] = {0};

  nyr_opt_begin(&f);
  emit_call(&f, NYIR_INST_F_NOALIAS, true);
  bool valid = nyr_ir_verify(&f, err, sizeof(err));
  nyr_ir_func_free(&f);

  nyr_opt_begin(&f);
  emit_call(&f, NYIR_INST_F_NOALIAS, false);
  err[0] = '\0';
  bool invalid_rejected = !nyr_ir_verify(&f, err, sizeof(err)) &&
                          strstr(err, "noalias fact requires") != NULL;
  nyr_ir_func_free(&f);

  nyr_opt_begin(&f);
  int value = nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_CONST_I64,
                                          .dst = -1,
                                          .a = -1,
                                          .b = -1,
                                          .c = -1,
                                          .d = -1,
                                          .e = -1,
                                          .f = -1,
                                          .imm = INT64_MAX,
                                          .flags = NYIR_INST_F_NOALIAS});
  nyr_opt_ret(&f, value);
  err[0] = '\0';
  bool non_call_rejected = !nyr_ir_verify(&f, err, sizeof(err)) &&
                           strstr(err, "noalias fact requires") != NULL;
  nyr_ir_func_free(&f);

  if (!valid || !invalid_rejected || !non_call_rejected)
    fprintf(stderr,
            "[noalias-contract] valid=%d invalid_rejected=%d "
            "non_call_rejected=%d: %s\n",
            valid, invalid_rejected, non_call_rejected, err);
  return valid && invalid_rejected && non_call_rejected ? 0 : 1;
}
