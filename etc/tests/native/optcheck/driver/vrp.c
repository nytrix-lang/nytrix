/*
 * Driver for nyir_vrp: value range propagation.
 *
 * Regression guard for the worklist overflow fix: the old worklist was
 * sized to the block count, prefilled with every block, and then grew
 * through successor pushes without bound — a heap write past the end on
 * wide CFGs.  The chain below (64 guarded blocks plus 64 side exits)
 * produces far more pushes than blocks.  Run with `make ASAN=1 run` so
 * a regression is a hard ASan abort instead of silent corruption.
 */
#include "util.h"
#include <limits.h>
#define CHAIN 64

static const nyir_inst_t *ny_find_def(const nyir_func_t *f, int value) {
  for (size_t i = 0; i < f->len; ++i)
    if (f->data[i].dst == value)
      return &f->data[i];
  return NULL;
}

static bool ny_exact_range(const nyir_inst_t *in, int64_t min, int64_t max) {
  return in && in->range.has_min && in->range.has_max &&
         in->range.min == min && in->range.max == max;
}

static bool ny_unknown_range(const nyir_inst_t *in) {
  return in && !in->range.has_min && !in->range.has_max;
}

static int ny_load_local(nyir_func_t *f, int64_t slot) {
  return nyir_emit(f, (nyir_inst_t){.op = NYIR_LOAD_LOCAL, .dst = -1,
                                    .a = -1, .b = -1, .c = -1, .d = -1,
                                    .e = -1, .f = -1, .imm = slot});
}

int main(void) {
  nyir_func_t f;
  ny_begin(&f);

  int zero = ny_const(&f, 0);
  int one = ny_const(&f, 1);
  int safe = ny_binop(&f, NYIR_ADD_I64, zero, one);
  int max = ny_const(&f, INT64_MAX);
  int overflow = ny_binop(&f, NYIR_ADD_I64, max, one);
  int cmp = ny_cmp(&f, safe, max, NYIR_CMP_LT);
  int unknown = ny_load_local(&f, 0);
  ny_ret(&f, safe);
  if (!ny_verify(&f, "vrp/bounds-before"))
    return 1;
  long long before = ny_eval(&f, "vrp/bounds-before");
  nyir_vrp(&f);
  if (!ny_verify(&f, "vrp/bounds-after"))
    return 1;
  if (!ny_exact_range(ny_find_def(&f, safe), 1, 1) ||
      !ny_exact_range(ny_find_def(&f, cmp), 1, 1) ||
      !ny_unknown_range(ny_find_def(&f, overflow)) ||
      !ny_unknown_range(ny_find_def(&f, unknown))) {
    fprintf(stderr, "[vrp] boundary lattice mismatch\n");
    return 1;
  }
  long long after = ny_eval(&f, "vrp/bounds-after");
  if (before != 1 || after != 1) {
    fprintf(stderr, "[vrp] boundary evaluation mismatch before=%lld after=%lld\n",
            before, after);
    return 1;
  }

  nyir_func_free(&f);
  ny_begin(&f);
  int acc = ny_const(&f, 0);
  for (int b = 0; b < CHAIN; ++b) {
    /*
     * always-false guard: VRP should prove the branch dead
     */
    int flag = ny_cmp(&f, acc, acc, NYIR_CMP_LT);
    ny_br_if(&f, flag, 100 + b); /* skipped side block */
    int bump = ny_const(&f, 1);
    acc = ny_binop(&f, NYIR_ADD_I64, acc, bump);
  }
  ny_ret(&f, acc);
  for (int b = 0; b < CHAIN; ++b) {
    ny_label(&f, 100 + b);
    ny_ret(&f, ny_const(&f, -1));
  }
  if (!ny_verify(&f, "vrp/verify-before"))
    return 1;
  before = ny_eval(&f, "vrp/before");
  nyir_vrp(&f);
  if (!ny_verify(&f, "vrp/verify-after"))
    return 1;
  after = ny_eval(&f, "vrp/after");
  if (ny_want_dump())
    nyir_dump(stdout, &f, "after-vrp");
  nyir_func_free(&f);
  ny_check("vrp", before, after, CHAIN);
  return 0;
}
