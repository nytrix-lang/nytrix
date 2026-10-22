/*
 * Driver for nyr_vrp: value range propagation.
 *
 * Regression guard for the worklist overflow fix: the old worklist was
 * sized to the block count, prefilled with every block, and then grew
 * through successor pushes without bound — a heap write past the end on
 * wide CFGs.  The chain below (64 guarded blocks plus 64 side exits)
 * produces far more pushes than blocks.  Run with `make ASAN=1 run` so
 * a regression is a hard ASan abort instead of silent corruption.
 */
#include "support.h"
#include <limits.h>
#define CHAIN 64

static const nyr_inst_t *ny_find_def(const nyr_func_t *f, int value) {
  for (size_t i = 0; i < f->len; ++i)
    if (f->data[i].dst == value)
      return &f->data[i];
  return NULL;
}

static bool ny_exact_range(const nyr_inst_t *in, int64_t min, int64_t max) {
  return in && in->range.has_min && in->range.has_max && in->range.min == min &&
         in->range.max == max;
}

static bool ny_unknown_range(const nyr_inst_t *in) {
  return in && !in->range.has_min && !in->range.has_max;
}

static int ny_load_local(nyr_func_t *f, int64_t slot) {
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

int main(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);

  int zero = nyr_opt_const(&f, 0);
  int one = nyr_opt_const(&f, 1);
  int safe = nyr_opt_binop(&f, NYIR_ADD_I64, zero, one);
  int max = nyr_opt_const(&f, INT64_MAX);
  int overflow = nyr_opt_binop(&f, NYIR_ADD_I64, max, one);
  int cmp = nyr_opt_cmp(&f, safe, max, NYIR_CMP_LT);
  int unknown = ny_load_local(&f, 0);
  nyr_opt_ret(&f, safe);
  if (!nyr_opt_verify(&f, "vrp/bounds-before"))
    return 1;
  long long before = nyr_opt_eval(&f, "vrp/bounds-before");
  nyr_vrp(&f);
  if (!nyr_opt_verify(&f, "vrp/bounds-after"))
    return 1;
  if (!ny_exact_range(ny_find_def(&f, safe), 1, 1) ||
      !ny_exact_range(ny_find_def(&f, cmp), 1, 1) ||
      !ny_unknown_range(ny_find_def(&f, overflow)) ||
      !ny_unknown_range(ny_find_def(&f, unknown))) {
    fprintf(stderr, "[vrp] boundary lattice mismatch\n");
    return 1;
  }
  long long after = nyr_opt_eval(&f, "vrp/bounds-after");
  if (before != 1 || after != 1) {
    fprintf(stderr,
            "[vrp] boundary evaluation mismatch before=%lld after=%lld\n",
            before, after);
    return 1;
  }

  nyr_ir_func_free(&f);

  /*
   * rt_argc returns the tagged nonnegative host argc.  Propagating its ABI
   * range through untagging lets LICM prove argc()-derived divisors nonzero.
   */
  nyr_opt_begin(&f);
  int tagged_argc = nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_CALL,
                                                .dst = -1,
                                                .a = -1,
                                                .b = -1,
                                                .c = -1,
                                                .d = -1,
                                                .e = -1,
                                                .f = -1,
                                                .symbol = "rt_argc",
                                                .effects = NYIR_EFFECT_CALL});
  int shift = nyr_opt_const(&f, 1);
  int argc_value = nyr_opt_binop(&f, NYIR_SAR_I64, tagged_argc, shift);
  nyr_opt_ret(&f, argc_value);
  if (!nyr_opt_verify(&f, "vrp/argc-before"))
    return 1;
  nyr_vrp(&f);
  if (!nyr_opt_verify(&f, "vrp/argc-after") ||
      !ny_exact_range(ny_find_def(&f, tagged_argc), 1, INT64_C(4294967295)) ||
      !ny_exact_range(ny_find_def(&f, argc_value), 0, INT32_MAX)) {
    fprintf(stderr, "[vrp] argc ABI range did not survive untagging\n");
    return 1;
  }
  nyr_ir_func_free(&f);

  nyr_opt_begin(&f);
  int acc = nyr_opt_const(&f, 0);
  for (int b = 0; b < CHAIN; ++b) {
    /*
     * always-false guard: VRP should prove the branch dead
     */
    int flag = nyr_opt_cmp(&f, acc, acc, NYIR_CMP_LT);
    nyr_opt_br_if(&f, flag, 100 + b); /* skipped side block */
    int bump = nyr_opt_const(&f, 1);
    acc = nyr_opt_binop(&f, NYIR_ADD_I64, acc, bump);
  }
  nyr_opt_ret(&f, acc);
  for (int b = 0; b < CHAIN; ++b) {
    nyr_opt_label(&f, 100 + b);
    nyr_opt_ret(&f, nyr_opt_const(&f, -1));
  }
  if (!nyr_opt_verify(&f, "vrp/verify-before"))
    return 1;
  before = nyr_opt_eval(&f, "vrp/before");
  nyr_vrp(&f);
  if (!nyr_opt_verify(&f, "vrp/verify-after"))
    return 1;
  after = nyr_opt_eval(&f, "vrp/after");
  if (nyr_opt_want_dump())
    nyr_dump(stdout, &f, "after-vrp");
  nyr_ir_func_free(&f);
  nyr_opt_check("vrp", before, after, CHAIN);
  return 0;
}
