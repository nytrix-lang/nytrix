/*
 * Cross-pass composition regression: exercise cleanup passes in different
 * orders and repeated pipelines, with verifier, evaluator, and fingerprint
 * checks at each boundary. Every permutation is tested on arithmetic,
 * wrapping overflow, and a select join.
 */
#include "code/backend/ir/opt/util.h"
#include "support.h"

#include <limits.h>

typedef bool (*opt_pass_t)(nyr_func_t *);
typedef bool (*ir_builder_t)(nyr_func_t *);

typedef struct pass_permutation_case {
  const char *name;
  ir_builder_t build;
  long long expected;
} pass_permutation_case_t;

static int64_t pass_order_cell = 17;

static bool build_arithmetic(nyr_func_t *f) {
  nyr_opt_begin(f);
  int four = nyr_opt_const(f, 4);
  int five = nyr_opt_const(f, 5);
  int left = nyr_opt_binop(f, NYIR_ADD_I64, four, five);
  int right = nyr_opt_binop(f, NYIR_ADD_I64, four, five);
  int sum = nyr_opt_binop(f, NYIR_ADD_I64, left, right);
  nyr_opt_ret(f, sum);
  return nyr_opt_verify(f, "pass-order/build");
}

static bool build_wrapping_overflow(nyr_func_t *f) {
  nyr_opt_begin(f);
  int max = nyr_opt_const(f, INT64_MAX);
  int one = nyr_opt_const(f, 1);
  int wrapped = nyr_opt_binop(f, NYIR_ADD_I64, max, one);
  nyr_opt_ret(f, wrapped);
  return nyr_opt_verify(f, "pass-order/overflow-build");
}

static bool build_memory_forwarding(nyr_func_t *f) {
  nyr_opt_begin(f);
  int zero = nyr_opt_const(f, 0);
  nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_STORE_LOCAL,
                             .dst = -1,
                             .a = zero,
                             .b = -1,
                             .c = -1,
                             .d = -1,
                             .e = -1,
                             .f = -1,
                             .imm = 0});
  int address = nyr_opt_const(f, (int64_t)(intptr_t)&pass_order_cell);
  int first = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                         .dst = -1,
                                         .a = address,
                                         .b = -1,
                                         .c = -1,
                                         .d = -1,
                                         .e = -1,
                                         .f = -1});
  int second = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                          .dst = -1,
                                          .a = address,
                                          .b = -1,
                                          .c = -1,
                                          .d = -1,
                                          .e = -1,
                                          .f = -1});
  int sum = nyr_opt_binop(f, NYIR_ADD_I64, first, second);
  nyr_opt_ret(f, sum);
  return nyr_opt_verify(f, "pass-order/memory-build");
}

static bool run_order(const char *name, ir_builder_t build,
                      const opt_pass_t *passes, size_t count,
                      long long expected_result) {
  nyr_func_t f;
  if (!build(&f))
    return false;
  long long expected = nyr_opt_eval(&f, name);
  if (expected != expected_result) {
    fprintf(stderr, "[%s] input result=%lld want=%lld\n", name, expected,
            expected_result);
    nyr_ir_func_free(&f);
    return false;
  }
  for (size_t i = 0; i < count; ++i) {
    if (!passes[i](&f) || !nyr_opt_verify(&f, name)) {
      fprintf(stderr, "[%s] pass %zu rejected or broke NYIR\n", name, i);
      nyr_ir_func_free(&f);
      return false;
    }
    if (nyr_opt_eval(&f, name) != expected) {
      fprintf(stderr, "[%s] semantic change after pass %zu\n", name, i);
      nyr_ir_func_free(&f);
      return false;
    }
  }
  uint64_t stable = nyr_debug_fingerprint(&f);
  bool converged = false;
  for (size_t round = 0; round < 8; ++round) {
    for (size_t i = 0; i < count; ++i) {
      if (!passes[i](&f) || !nyr_opt_verify(&f, name)) {
        fprintf(stderr, "[%s] repeat %zu pass %zu rejected or broke NYIR\n",
                name, round, i);
        nyr_ir_func_free(&f);
        return false;
      }
    }
    uint64_t next = nyr_debug_fingerprint(&f);
    if (next == stable) {
      converged = true;
      break;
    }
    stable = next;
    if (nyr_opt_eval(&f, name) != expected) {
      fprintf(stderr, "[%s] semantic change in repeated pipeline\n", name);
      nyr_ir_func_free(&f);
      return false;
    }
  }
  bool ok = converged && nyr_opt_eval(&f, name) == expected;
  if (!ok)
    fprintf(stderr, "[%s] repeated pipeline did not reach a fixed point\n",
            name);
  nyr_ir_func_free(&f);
  return ok;
}

static bool fingerprint_tracks_cfg_edges(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  nyr_opt_label(&f, 1);
  nyr_opt_br(&f, 2);
  nyr_opt_label(&f, 2);
  int one = nyr_opt_const(&f, 1);
  nyr_opt_ret(&f, one);
  nyr_opt_label(&f, 3);
  int two = nyr_opt_const(&f, 2);
  nyr_opt_ret(&f, two);
  if (!nyr_opt_verify(&f, "fingerprint/before")) {
    nyr_ir_func_free(&f);
    return false;
  }
  uint64_t before = nyr_debug_fingerprint(&f);
  f.data[1].imm = 3;
  bool ok = nyr_opt_verify(&f, "fingerprint/after") &&
            before != nyr_debug_fingerprint(&f) &&
            nyr_opt_eval(&f, "fingerprint") == 2;
  if (!ok)
    fprintf(stderr, "[fingerprint] CFG edge mutation was not observable\n");
  nyr_ir_func_free(&f);
  return ok;
}

static bool fingerprint_tracks_range_facts(void) {
  nyr_func_t f;
  if (!build_arithmetic(&f))
    return false;
  uint64_t before = nyr_debug_fingerprint(&f);
  /*
   * The literal remains valid in [3, 4], but a range-sensitive transform
   * can make a different decision from this fact.
   */
  f.data[0].range.min = 3;
  bool ok = nyr_opt_verify(&f, "fingerprint/range") &&
            before != nyr_debug_fingerprint(&f) &&
            nyr_opt_eval(&f, "range") == 18;
  if (!ok)
    fprintf(stderr, "[fingerprint] range-fact mutation was not observable\n");
  nyr_ir_func_free(&f);
  return ok;
}

static bool fingerprint_tracks_debug_locations(void) {
  nyr_func_t f;
  if (!build_arithmetic(&f))
    return false;
  static const char file[] = "pass-order-debug.ny";
  f.data[0].debug = (nyr_debug_loc_t){.file = file, .line = 3, .column = 7};
  uint64_t before = nyr_debug_fingerprint(&f);
  f.data[0].debug.column = 8;
  bool ok = nyr_opt_verify(&f, "fingerprint/debug-location") &&
            before != nyr_debug_fingerprint(&f) &&
            nyr_opt_eval(&f, "debug-location") == 18;
  uint64_t with_file = nyr_debug_fingerprint(&f);
  f.data[0].debug.file = NULL;
  ok = ok && with_file != nyr_debug_fingerprint(&f);
  if (!ok)
    fprintf(stderr,
            "[fingerprint] source-location mutation was not observable\n");
  nyr_ir_func_free(&f);
  return ok;
}

static bool build_two_loops(nyr_func_t *f) {
  nyr_opt_build_two_loops(f, 3, 3);
  return nyr_opt_verify(f, "pass-order/two-loops");
}

/*
 * Representation boundary: f64 arithmetic feeding an f64→i64 step, so the
 * function returns i64 while the middle passes see mixed float/int facts.
 */
static bool build_f64_math(nyr_func_t *f) {
  nyr_opt_begin(f);
  int a = nyr_opt_const_f64(f, 2.5);
  int b = nyr_opt_const_f64(f, 4.0);
  int sum = nyr_opt_binop(f, NYIR_ADD_F64, a, b);
  int prod = nyr_opt_binop(f, NYIR_MUL_F64, sum, a);
  int truncated = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_F64_TO_I64,
                                             .dst = -1,
                                             .a = prod,
                                             .b = -1,
                                             .c = -1,
                                             .d = -1,
                                             .e = -1,
                                             .f = -1});
  nyr_opt_ret(f, truncated);
  return nyr_opt_verify(f, "pass-order/f64");
}

/*
 * Branch-join: a phi merging two arms through a select — exercises
 * copy_prop/phi_elim/dce composition on join shapes.
 */
static bool build_select_join(nyr_func_t *f) {
  nyr_opt_begin(f);
  int cond = nyr_opt_const(f, 1);
  int a = nyr_opt_const(f, 10);
  int b = nyr_opt_const(f, 20);
  int arm_a = nyr_opt_binop(f, NYIR_ADD_I64, a, b);
  int arm_b = nyr_opt_binop(f, NYIR_MUL_I64, a, b);
  int sel = nyr_opt_select(f, cond, arm_a, arm_b);
  nyr_opt_ret(f, sel);
  return nyr_opt_verify(f, "pass-order/select");
}

static const opt_pass_t cleanup_passes[] = {nyr_const_fold, nyr_copy_prop,
                                            nyr_cse, nyr_adce, nyr_dce};

static const pass_permutation_case_t cleanup_cases[] = {
    {"arithmetic", build_arithmetic, 18},
    {"wrapping-overflow", build_wrapping_overflow, INT64_MIN},
    {"select-join", build_select_join, 30},
    {"memory-forwarding", build_memory_forwarding, 34},
    {"f64-conversion", build_f64_math, 16},
    {"loop-carried", build_two_loops, 209},
};

static size_t cleanup_permutation_count;

static bool run_cleanup_permutations(size_t depth, opt_pass_t *permutation,
                                     bool *used) {
  const size_t pass_count = sizeof(cleanup_passes) / sizeof(cleanup_passes[0]);
  if (depth == pass_count) {
    char order_name[32] = "perm-";
    size_t off = 5;
    for (size_t i = 0; i < pass_count && off < sizeof(order_name); ++i) {
      size_t pass_id = 0;
      while (pass_id < pass_count && cleanup_passes[pass_id] != permutation[i])
        ++pass_id;
      off += (size_t)snprintf(order_name + off, sizeof(order_name) - off, "%zu",
                              pass_id);
    }
    ++cleanup_permutation_count;
    for (size_t c = 0; c < sizeof(cleanup_cases) / sizeof(cleanup_cases[0]);
         ++c) {
      char case_name[64];
      snprintf(case_name, sizeof(case_name), "%s-%s", order_name,
               cleanup_cases[c].name);
      if (!run_order(case_name, cleanup_cases[c].build, permutation, pass_count,
                     cleanup_cases[c].expected))
        return false;
    }
    return true;
  }
  for (size_t i = 0; i < pass_count; ++i) {
    if (used[i])
      continue;
    used[i] = true;
    permutation[depth] = cleanup_passes[i];
    if (!run_cleanup_permutations(depth + 1, permutation, used))
      return false;
    used[i] = false;
  }
  return true;
}

int main(void) {
  /*
   * mem2reg leaves dead local stores behind for dce; compact drains the
   * NOPs — the documented pipeline coupling, previously unexercised.
   */
  static const opt_pass_t promotion_order[] = {nyr_mem2reg, nyr_dce,
                                               nyr_compact};
  /*
   * sccp marks constant edges, cfg_simplify removes them, const_fold mops
   * up arithmetic sccp declines — the production sccp→cfg_simplify→dce
   * adjacency.
   */
  static const opt_pass_t sccp_order[] = {nyr_sccp, nyr_cfg_simplify,
                                          nyr_const_fold, nyr_dce};
  /*
   * Loop family: iv_elim rewrites accumulations into closed form, strength
   * reduce consumes the mul-by-constant shapes, copy_prop coalesces the
   * copies both introduce.
   */
  static const opt_pass_t loop_order[] = {nyr_iv_elim, nyr_strength_reduce,
                                          nyr_copy_prop, nyr_dce,
                                          nyr_compact};
  static const opt_pass_t order_a[] = {nyr_const_fold, nyr_copy_prop,
                                       nyr_cse,        nyr_adce,
                                       nyr_dce,        nyr_compact};
  static const opt_pass_t order_b[] = {nyr_cse,     nyr_const_fold,
                                       nyr_compact, nyr_adce,
                                       nyr_dce,     nyr_copy_prop};
  static const opt_pass_t order_c[] = {nyr_copy_prop, nyr_adce,
                                       nyr_dce,       nyr_const_fold,
                                       nyr_cse,       nyr_compact};
  static const opt_pass_t memory_order_a[] = {nyr_alias_analysis,
                                              nyr_redundant_load_elim,
                                              nyr_copy_prop,
                                              nyr_adce,
                                              nyr_dce,
                                              nyr_compact};
  static const opt_pass_t memory_order_b[] = {
      nyr_copy_prop, nyr_alias_analysis,      nyr_adce,
      nyr_compact,   nyr_redundant_load_elim, nyr_dce};
  if (!run_order("order-a", build_arithmetic, order_a,
                 sizeof(order_a) / sizeof(order_a[0]), 18) ||
      !run_order("order-b", build_arithmetic, order_b,
                 sizeof(order_b) / sizeof(order_b[0]), 18) ||
      !run_order("order-c", build_arithmetic, order_c,
                 sizeof(order_c) / sizeof(order_c[0]), 18) ||
      !run_order("memory-order-a", build_memory_forwarding, memory_order_a,
                 sizeof(memory_order_a) / sizeof(memory_order_a[0]), 34) ||
      !run_order("memory-order-b", build_memory_forwarding, memory_order_b,
                 sizeof(memory_order_b) / sizeof(memory_order_b[0]), 34) ||
      !run_order("overflow-order", build_wrapping_overflow, order_a,
                 sizeof(order_a) / sizeof(order_a[0]), INT64_MIN) ||
      !run_order("promotion-order", build_memory_forwarding, promotion_order,
                 sizeof(promotion_order) / sizeof(promotion_order[0]), 34) ||
      !run_order("sccp-order", build_arithmetic, sccp_order,
                 sizeof(sccp_order) / sizeof(sccp_order[0]), 18) ||
      !run_order("loop-order", build_two_loops, loop_order,
                 sizeof(loop_order) / sizeof(loop_order[0]), 209) ||
      !run_order("f64-order", build_f64_math, sccp_order,
                 sizeof(sccp_order) / sizeof(sccp_order[0]), 16) ||
      !run_order("select-order", build_select_join, promotion_order,
                 sizeof(promotion_order) / sizeof(promotion_order[0]), 30) ||
      !fingerprint_tracks_cfg_edges() || !fingerprint_tracks_range_facts() ||
      !fingerprint_tracks_debug_locations())
    return 1;
  opt_pass_t permutation[sizeof(cleanup_passes) / sizeof(cleanup_passes[0])];
  bool used[sizeof(cleanup_passes) / sizeof(cleanup_passes[0])] = {false};
  if (!run_cleanup_permutations(0, permutation, used) ||
      cleanup_permutation_count != 120) {
    fprintf(stderr, "[pass-order] tested %zu cleanup permutations; want 120\n",
            cleanup_permutation_count);
    return 1;
  }
  puts("[pass-order] ok permutations=11 fixed-point=yes "
       "cleanup-permutations=120×6 "
       "alias/memory/overflow/f64/loop/promotion/sccp=yes "
       "cfg/range/debug-fingerprints=yes");
  return 0;
}
