/*
 * Regression for debug metadata through self-recursive tail-call lowering.
 */
#include "support.h"

static bool test_unlabeled_entry(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  f.param_count = 1;
  f.param_types = calloc(1, sizeof(*f.param_types));
  if (!f.param_types) {
    nyr_ir_func_free(&f);
    return false;
  }
  f.param_types[0] = NYIR_PARAM_I64;

  int arg = nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                         .dst = -1,
                                         .a = -1,
                                         .b = -1,
                                         .c = -1,
                                         .d = -1,
                                         .e = -1,
                                         .f = -1,
                                         .imm = 0});
  if (arg < 0) {
    nyr_ir_func_free(&f);
    return false;
  }
  nyr_opt_label(&f, 7);
  int call = nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_CALL,
                                           .dst = -1,
                                           .a = arg,
                                           .b = -1,
                                           .c = -1,
                                           .d = -1,
                                           .e = -1,
                                           .f = -1,
                                           .imm = 1,
                                           .symbol = "recur"});
  if (call < 0) {
    nyr_ir_func_free(&f);
    return false;
  }
  nyr_opt_ret(&f, call);

  bool ok = nyr_opt_verify(&f, "tco/entry-before") &&
            nyr_tail_call(&f, "recur") &&
            nyr_opt_verify(&f, "tco/entry-after") && f.len > 1;
  /*
   * The function entry is either labeled directly or split off as
   * `br L; L: <old entry>`; the recursive branch must target that label,
   * never the unrelated later block L7.
   */
  int64_t entry = -1;
  if (ok && f.data[0].op == NYIR_LABEL)
    entry = f.data[0].imm;
  else if (ok && f.data[0].op == NYIR_BR && f.data[1].op == NYIR_LABEL &&
           f.data[1].imm == f.data[0].imm)
    entry = f.data[1].imm;
  ok = ok && entry >= 0 && entry != 7;
  bool branches_to_entry = false;
  for (size_t i = 1; ok && i < f.len; ++i) {
    if (f.data[i].op == NYIR_BR && f.data[i].imm == entry)
      branches_to_entry = true;
  }
  ok = ok && branches_to_entry;
  if (!ok) {
    fprintf(stderr, "[tail-call] recursive branch skipped the function entry\n");
    nyr_dump(stderr, &f, "tco/entry-failure");
  }
  nyr_ir_func_free(&f);
  return ok;
}

static bool test_parallel_args_and_nop(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  f.param_count = 2;
  f.param_types = calloc(2, sizeof(*f.param_types));
  if (!f.param_types)
    return false;
  f.param_types[0] = f.param_types[1] = NYIR_PARAM_I64;
  int args[2];
  for (int i = 0; i < 2; ++i)
    args[i] = nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                            .dst = -1, .a = -1, .b = -1,
                                            .imm = i});
  int call = nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_CALL, .dst = -1,
                                          .a = args[1], .b = args[0],
                                          .c = -1, .d = -1, .e = -1,
                                          .f = -1, .imm = 2,
                                          .symbol = "swap"});
  nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_NOP, .dst = -1});
  nyr_opt_ret(&f, call);
  bool ok = nyr_opt_verify(&f, "tco/swap-before") &&
            nyr_tail_call(&f, "swap") &&
            nyr_opt_verify(&f, "tco/swap-after");
  size_t stores = 0;
  for (size_t i = 0; ok && i < f.len; ++i) {
    const nyr_inst_t *in = &f.data[i];
    if (in->op == NYIR_CALL || in->op == NYIR_RET)
      ok = false;
    if (in->op == NYIR_STORE_LOCAL) {
      ok = stores < 2 && in->imm == (int64_t)stores &&
           in->a == args[1 - stores];
      ++stores;
    }
  }
  ok = ok && stores == 2;
  if (!ok)
    nyr_dump(stderr, &f, "tco/swap-failure");
  nyr_ir_func_free(&f);
  return ok;
}

int main(void) {
  static const char source[] = "tail-call-origin.ny";
  nyr_func_t f;
  nyr_opt_begin(&f);
  f.param_count = 1;
  f.param_types = calloc(1, sizeof(*f.param_types));
  if (!f.param_types)
    return 2;
  f.param_types[0] = NYIR_PARAM_I64;

  nyr_opt_label(&f, 7);
  int arg = nyr_opt_const(&f, 9);
  size_t call_at = f.len;
  nyr_ir_emit(&f,
            (nyr_inst_t){.op = NYIR_CALL,
                          .dst = -1,
                          .a = arg,
                          .b = -1,
                          .c = -1,
                          .d = -1,
                          .e = -1,
                          .f = -1,
                          .imm = 1,
                          .symbol = "recur",
                          .debug = {.file = source, .line = 12, .column = 5}});
  nyr_opt_ret(&f, f.data[call_at].dst);

  bool ok = nyr_opt_verify(&f, "tco/debug-before") &&
            nyr_tail_call(&f, "recur") &&
            nyr_opt_verify(&f, "tco/debug-after");
  size_t generated = call_at;
  if (ok) {
    ok = f.data[generated].op == NYIR_STORE_LOCAL &&
         f.data[generated + 1].op == NYIR_BR &&
         f.data[generated].debug.file == source &&
         f.data[generated].debug.line == 12 &&
         f.data[generated].debug.column == 5 &&
         f.data[generated + 1].debug.file == source &&
         f.data[generated + 1].debug.line == 12 &&
         f.data[generated + 1].debug.column == 5;
  }
  if (!ok) {
    fprintf(stderr, "[tail-call] generated instructions lost debug location\n");
    nyr_dump(stderr, &f, "tco/debug-failure");
  }
  nyr_ir_func_free(&f);
  return ok && test_unlabeled_entry() && test_parallel_args_and_nop() ? 0 : 1;
}
