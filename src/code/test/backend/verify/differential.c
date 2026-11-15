/*
 * Seeded differential fuzzing for scalar integer expressions.
 *
 * For each seed this builds one random expression over local slot 0, clones it,
 * and runs the real scalar pipeline (e-graph, constant folding, peephole, CSE)
 * on the clone. The reference interpreter must agree on every input in the
 * domain for both forms. On a mismatch the generator is retried at a smaller
 * depth with the same seed, so the report is a minimized expression rather than
 * a random tree; the printed seed reproduces it exactly.
 *
 * Operations are restricted to those that cannot trap: shifts use a masked
 * count and division is excluded. That keeps a mismatch a compiler defect
 * rather than an undefined-behavior disagreement, which is the distinction the
 * verification record depends on.
 *
 * The three outcomes are kept separate on purpose. "The optimizer changed the
 * answer" (CASE_MISMATCH) is a semantic defect; "the optimizer produced IR the
 * verifier rejects" (CASE_INVALID_IR) is a structural defect; a harness failure
 * to build or clone is neither and must never be counted as a miscompile.
 */
#include "opt.h"

extern bool nyr_egraph_rewrite(nyr_func_t *f);
extern bool nyr_const_fold(nyr_func_t *f);
extern bool nyr_peephole(nyr_func_t *f);
extern bool nyr_cse(nyr_func_t *f);

static const int64_t k_domain[] = {
    0,     1,     2,     3,     7,     8,      15,      16,
    31,    63,    127,   255,   1024,  -1,     -2,      -7,
    -256,  INT64_C(1) << 32,     INT64_MAX};

static const size_t k_domain_len = sizeof(k_domain) / sizeof(k_domain[0]);
static const size_t k_seeds = 300;
static const size_t k_max_depth = 5;
static const size_t k_max_reports = 8;

typedef struct {
  uint64_t state;
} rng_t;

static uint64_t rng_next(rng_t *r) {
  uint64_t z = (r->state += UINT64_C(0x9e3779b97f4a7c15));
  z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
  z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
  return z ^ (z >> 31);
}

static unsigned rng_below(rng_t *r, unsigned limit) {
  return (unsigned)(rng_next(r) % limit);
}

static nyr_op_t rng_op(rng_t *r) {
  static const nyr_op_t ops[] = {
      NYIR_ADD_I64, NYIR_SUB_I64, NYIR_MUL_I64, NYIR_AND_I64,
      NYIR_OR_I64,  NYIR_XOR_I64, NYIR_SHL_I64, NYIR_SAR_I64,
  };
  return ops[rng_below(r, (unsigned)(sizeof(ops) / sizeof(ops[0])))];
}

/* Leaves stay small so constant folding is exercised, not just tree shape. */
static int gen_leaf(nyr_func_t *f, rng_t *r) {
  if (rng_below(r, 3) == 0)
    return nyr_opt_emit_imm(f, NYIR_LOAD_LOCAL, 0);
  return nyr_opt_const(f, (int64_t)rng_below(r, 40) - 20);
}

static int gen_expr(nyr_func_t *f, rng_t *r, size_t depth) {
  if (depth == 0)
    return gen_leaf(f, r);
  nyr_op_t op = rng_op(r);
  int lhs = gen_expr(f, r, depth - 1);
  int rhs = gen_expr(f, r, depth - 1);
  if (op == NYIR_SHL_I64 || op == NYIR_SAR_I64) {
    /* Mask the count: an oversized shift is not the behavior under test. */
    rhs = nyr_opt_binop(f, NYIR_AND_I64, rhs, nyr_opt_const(f, 63));
  }
  return nyr_opt_binop(f, op, lhs, rhs);
}

static void build_program(nyr_func_t *f, size_t depth, size_t seed) {
  nyr_opt_begin(f);
  rng_t rng = {.state = (uint64_t)seed * UINT64_C(2654435761)};
  int root = gen_expr(f, &rng, depth);
  nyr_opt_ret(f, root);
}

static bool eval_at(nyr_func_t *f, int64_t input, int64_t *out) {
  int64_t locals[8] = {input, 0, 0, 0, 0, 0, 0, 0};
  return nyr_opt_eval_locals(f, "differential", locals, 8, out);
}

/*
 * Run the same pipeline shape the optimizer uses. Each stage must keep the
 * function verifiable; a stage that breaks the IR is a structural defect even
 * when the evaluated result happens to survive.
 */
static bool optimize(nyr_func_t *f) {
  for (int pass = 0; pass < 2; ++pass) {
    (void)nyr_egraph_rewrite(f);
    (void)nyr_const_fold(f);
    (void)nyr_peephole(f);
    (void)nyr_cse(f);
    if (!nyr_opt_verify(f, "differential-opt"))
      return false;
  }
  return true;
}

typedef enum {
  CASE_AGREE = 0,
  CASE_MISMATCH,
  CASE_INVALID_IR,
  CASE_HARNESS_ERROR,
} case_result_t;

static case_result_t run_case(size_t depth, size_t seed, int64_t *witness,
                              int64_t *before_out, int64_t *after_out) {
  nyr_func_t original;
  nyr_func_t optimized = {0}; /* nyr_ir_func_clone requires an empty target */
  build_program(&original, depth, seed);
  if (!nyr_opt_verify(&original, "differential-original")) {
    nyr_ir_func_free(&original);
    return CASE_HARNESS_ERROR;
  }
  if (!nyr_ir_func_clone(&original, &optimized)) {
    fprintf(stderr, "[differential] seed=%zu depth=%zu: clone failed\n", seed,
            depth);
    nyr_ir_func_free(&original);
    return CASE_HARNESS_ERROR;
  }
  if (!optimize(&optimized)) {
    fprintf(stderr,
            "[differential] seed=%zu depth=%zu: optimizer produced invalid "
            "IR\n",
            seed, depth);
    nyr_ir_func_free(&original);
    nyr_ir_func_free(&optimized);
    return CASE_INVALID_IR;
  }
  case_result_t result = CASE_AGREE;
  for (size_t i = 0; i < k_domain_len && result == CASE_AGREE; ++i) {
    int64_t before = 0, after = 0;
    if (!eval_at(&original, k_domain[i], &before) ||
        !eval_at(&optimized, k_domain[i], &after)) {
      fprintf(stderr, "[differential] seed=%zu depth=%zu: eval failed\n", seed,
              depth);
      result = CASE_HARNESS_ERROR;
      break;
    }
    if (before != after) {
      if (witness)
        *witness = k_domain[i];
      if (before_out)
        *before_out = before;
      if (after_out)
        *after_out = after;
      result = CASE_MISMATCH;
    }
  }
  nyr_ir_func_free(&original);
  nyr_ir_func_free(&optimized);
  return result;
}

int main(void) {
  size_t mismatches = 0, invalid_ir = 0, harness_errors = 0;
  for (size_t seed = 1; seed <= k_seeds; ++seed) {
    int64_t witness = 0, before = 0, after = 0;
    case_result_t result = run_case(k_max_depth, seed, &witness, &before, &after);
    if (result == CASE_AGREE)
      continue;
    if (result == CASE_INVALID_IR) {
      ++invalid_ir;
      if (invalid_ir >= k_max_reports)
        break;
      continue;
    }
    if (result == CASE_HARNESS_ERROR) {
      ++harness_errors;
      if (harness_errors >= k_max_reports)
        break;
      continue;
    }
    ++mismatches;
    /*
     * Shrink: the first smaller depth that still mismatches is a smaller
     * reproducer for the same seed. Only mismatches shrink; a structural or
     * harness failure has no smaller semantic witness.
     */
    size_t minimal = k_max_depth;
    for (size_t depth = 1; depth < k_max_depth; ++depth) {
      int64_t w2 = 0, b2 = 0, a2 = 0;
      if (run_case(depth, seed, &w2, &b2, &a2) == CASE_MISMATCH) {
        minimal = depth;
        witness = w2;
        before = b2;
        after = a2;
        break;
      }
    }
    fprintf(stderr,
            "[differential] MISMATCH seed=%zu depth=%zu x=%lld original=%lld "
            "optimized=%lld\n",
            seed, minimal, (long long)witness, (long long)before,
            (long long)after);
    if (mismatches >= k_max_reports)
      break;
  }
  if (mismatches || invalid_ir || harness_errors) {
    fprintf(stderr,
            "[differential] %zu mismatches, %zu invalid-IR, %zu harness "
            "errors over %zu seeded programs\n",
            mismatches, invalid_ir, harness_errors, k_seeds);
    return 1;
  }
  printf("[differential] %zu seeded programs x %zu inputs: unoptimized and "
         "optimized IR agree\n",
         k_seeds, k_domain_len);
  return 0;
}
