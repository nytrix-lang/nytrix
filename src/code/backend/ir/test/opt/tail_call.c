/*
 * Regression for debug metadata through self-recursive tail-call lowering.
 */
#include "support.h"

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
  return ok ? 0 : 1;
}
