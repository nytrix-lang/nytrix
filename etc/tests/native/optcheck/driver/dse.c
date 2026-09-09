/*
 * Regression driver for path-sensitive direct-local dead-store elimination.
 */
#include "util.h"

static void ny_store_local(nyir_func_t *f, int value, int slot) {
  nyir_emit(f, (nyir_inst_t){.op = NYIR_STORE_LOCAL, .dst = -1, .a = value,
                             .b = -1, .c = -1, .d = -1, .e = -1, .f = -1,
                             .imm = slot});
}

static int ny_load_local(nyir_func_t *f, int slot) {
  return nyir_emit(f, (nyir_inst_t){.op = NYIR_LOAD_LOCAL, .dst = -1,
                                    .a = -1, .b = -1, .c = -1, .d = -1,
                                    .e = -1, .f = -1, .imm = slot});
}

static size_t ny_nops(const nyir_func_t *f) {
  size_t count = 0;
  for (size_t i = 0; i < f->len; ++i)
    count += f->data[i].op == NYIR_NOP;
  return count;
}

static void ny_build_overwritten_fork(nyir_func_t *f) {
  ny_begin(f);
  int dead = ny_const(f, 42);
  int live = ny_const(f, 7);
  int branch = ny_const(f, 1);
  ny_store_local(f, dead, 0);
  ny_br_if(f, branch, 20);
  ny_label(f, 10);
  ny_store_local(f, live, 0);
  ny_br(f, 30);
  ny_label(f, 20);
  ny_store_local(f, live, 0);
  ny_br(f, 30);
  ny_label(f, 30);
  ny_ret(f, ny_load_local(f, 0));
}

static void ny_build_observed_fork(nyir_func_t *f) {
  ny_begin(f);
  int live = ny_const(f, 42);
  int replacement = ny_const(f, 7);
  int branch = ny_const(f, 1);
  ny_store_local(f, live, 0);
  ny_br_if(f, branch, 20);
  ny_label(f, 10);
  ny_store_local(f, replacement, 0);
  ny_br(f, 30);
  ny_label(f, 20);
  ny_ret(f, ny_load_local(f, 0));
  ny_label(f, 30);
  ny_ret(f, ny_load_local(f, 0));
}

static int ny_check_overwritten_fork(void) {
  nyir_func_t f;
  ny_build_overwritten_fork(&f);
  if (!ny_verify(&f, "dse/overwritten-before"))
    return 1;
  long long before = ny_eval(&f, "dse/overwritten-before");
  size_t nops = ny_nops(&f);
  if (!nyir_dead_store_elim(&f) || !ny_verify(&f, "dse/overwritten-after")) {
    nyir_func_free(&f);
    return 1;
  }
  long long after = ny_eval(&f, "dse/overwritten-after");
  if (ny_nops(&f) != nops + 1) {
    fprintf(stderr, "[dse] expected the fork predecessor store to disappear\n");
    nyir_func_free(&f);
    return 1;
  }
  nyir_func_free(&f);
  ny_check("dse/overwritten", before, after, 7);
  return 0;
}

static int ny_check_observed_fork(void) {
  nyir_func_t f;
  ny_build_observed_fork(&f);
  if (!ny_verify(&f, "dse/observed-before"))
    return 1;
  long long before = ny_eval(&f, "dse/observed-before");
  size_t nops = ny_nops(&f);
  if (!nyir_dead_store_elim(&f) || !ny_verify(&f, "dse/observed-after")) {
    nyir_func_free(&f);
    return 1;
  }
  long long after = ny_eval(&f, "dse/observed-after");
  if (ny_nops(&f) != nops) {
    fprintf(stderr, "[dse] eliminated a store with an observing successor\n");
    nyir_func_free(&f);
    return 1;
  }
  nyir_func_free(&f);
  ny_check("dse/observed", before, after, 42);
  return 0;
}

int main(void) {
  return ny_check_overwritten_fork() || ny_check_observed_fork();
}
