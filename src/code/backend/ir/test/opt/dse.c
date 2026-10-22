/*
 * Regression driver for path-sensitive direct-local dead-store elimination.
 */
#include "support.h"

static void ny_store_local(nyr_func_t *f, int value, int slot) {
  nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_STORE_LOCAL,
                             .dst = -1,
                             .a = value,
                             .b = -1,
                             .c = -1,
                             .d = -1,
                             .e = -1,
                             .f = -1,
                             .imm = slot});
}

static int ny_load_local(nyr_func_t *f, int slot) {
  return nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                    .dst = -1,
                                    .a = -1,
                                    .b = -1,
                                    .c = -1,
                                    .d = -1,
                                    .e = -1,
                                    .f = -1,
                                    .imm = slot});
}

static size_t ny_nops(const nyr_func_t *f) {
  size_t count = 0;
  for (size_t i = 0; i < f->len; ++i)
    count += f->data[i].op == NYIR_NOP;
  return count;
}

static void ny_build_overwritten_fork(nyr_func_t *f) {
  nyr_opt_begin(f);
  int dead = nyr_opt_const(f, 42);
  int live = nyr_opt_const(f, 7);
  int branch = nyr_opt_const(f, 1);
  ny_store_local(f, dead, 0);
  nyr_opt_br_if(f, branch, 20);
  nyr_opt_label(f, 10);
  ny_store_local(f, live, 0);
  nyr_opt_br(f, 30);
  nyr_opt_label(f, 20);
  ny_store_local(f, live, 0);
  nyr_opt_br(f, 30);
  nyr_opt_label(f, 30);
  nyr_opt_ret(f, ny_load_local(f, 0));
}

static void ny_build_observed_fork(nyr_func_t *f) {
  nyr_opt_begin(f);
  int live = nyr_opt_const(f, 42);
  int replacement = nyr_opt_const(f, 7);
  int branch = nyr_opt_const(f, 1);
  ny_store_local(f, live, 0);
  nyr_opt_br_if(f, branch, 20);
  nyr_opt_label(f, 10);
  ny_store_local(f, replacement, 0);
  nyr_opt_br(f, 30);
  nyr_opt_label(f, 20);
  nyr_opt_ret(f, ny_load_local(f, 0));
  nyr_opt_label(f, 30);
  nyr_opt_ret(f, ny_load_local(f, 0));
}

static int ny_check_overwritten_fork(void) {
  nyr_func_t f;
  ny_build_overwritten_fork(&f);
  if (!nyr_opt_verify(&f, "dse/overwritten-before"))
    return 1;
  long long before = nyr_opt_eval(&f, "dse/overwritten-before");
  size_t nops = ny_nops(&f);
  if (!nyr_dead_store_elim(&f) ||
      !nyr_opt_verify(&f, "dse/overwritten-after")) {
    nyr_ir_func_free(&f);
    return 1;
  }
  long long after = nyr_opt_eval(&f, "dse/overwritten-after");
  if (ny_nops(&f) != nops + 1) {
    fprintf(stderr, "[dse] expected the fork predecessor store to disappear\n");
    nyr_ir_func_free(&f);
    return 1;
  }
  nyr_ir_func_free(&f);
  nyr_opt_check("dse/overwritten", before, after, 7);
  return 0;
}

static int ny_check_observed_fork(void) {
  nyr_func_t f;
  ny_build_observed_fork(&f);
  if (!nyr_opt_verify(&f, "dse/observed-before"))
    return 1;
  long long before = nyr_opt_eval(&f, "dse/observed-before");
  size_t nops = ny_nops(&f);
  if (!nyr_dead_store_elim(&f) || !nyr_opt_verify(&f, "dse/observed-after")) {
    nyr_ir_func_free(&f);
    return 1;
  }
  long long after = nyr_opt_eval(&f, "dse/observed-after");
  if (ny_nops(&f) != nops) {
    fprintf(stderr, "[dse] eliminated a store with an observing successor\n");
    nyr_ir_func_free(&f);
    return 1;
  }
  nyr_ir_func_free(&f);
  nyr_opt_check("dse/observed", before, after, 42);
  return 0;
}

static int ny_check_expanded_call_argument_escape(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int zero = nyr_opt_const(&f, 0);
  int value = nyr_opt_const(&f, 73);
  ny_store_local(&f, zero,
                 0); /* Keep DSE's function-wide allocation pass on. */
  int allocation =
      nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_ALLOCA,
                                  .dst = -1,
                                  .a = -1,
                                  .b = -1,
                                  .c = -1,
                                  .d = -1,
                                  .e = -1,
                                  .f = -1,
                                  .imm = 8,
                                  .scalar_class = NYIR_SCALAR_POINTER});
  size_t store_at = f.len;
  nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_STORE_I64,
                              .dst = -1,
                              .a = allocation,
                              .b = -1,
                              .c = value,
                              .d = -1,
                              .e = -1,
                              .f = -1,
                              .effects = NYIR_EFFECT_WRITE_MEMORY |
                                         NYIR_EFFECT_MAY_TRAP});
  int args[7] = {zero, zero, zero, zero, zero, zero, allocation};
  int *extra = malloc(sizeof(*extra));
  if (!extra) {
    nyr_ir_func_free(&f);
    return 1;
  }
  extra[0] = args[6];
  nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_CALL,
                              .dst = -1,
                              .a = args[0],
                              .b = args[1],
                              .c = args[2],
                              .d = args[3],
                              .e = args[4],
                              .f = args[5],
                              .imm = 7,
                              .symbol = "unknown_observer",
                              .extra_args = extra,
                              .extra_args_len = 1,
                              .effects = NYIR_EFFECT_CALL |
                                         NYIR_EFFECT_UNKNOWN_SIDE_EFFECT});
  nyr_opt_ret(&f, zero);
  if (!nyr_opt_verify(&f, "dse/expanded-escape-before")) {
    nyr_ir_func_free(&f);
    return 1;
  }
  if (!nyr_dead_store_elim(&f) ||
      !nyr_opt_verify(&f, "dse/expanded-escape-after") ||
      f.data[store_at].op != NYIR_STORE_I64) {
    fprintf(
        stderr,
        "[dse] removed a store to an allocation passed in expanded arg 7\n");
    nyr_dump(stderr, &f, "dse/expanded-escape");
    nyr_ir_func_free(&f);
    return 1;
  }
  nyr_ir_func_free(&f);
  return 0;
}

int main(void) {
  return ny_check_overwritten_fork() || ny_check_observed_fork() ||
         ny_check_expanded_call_argument_escape();
}
