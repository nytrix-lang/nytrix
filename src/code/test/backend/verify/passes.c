/*
 * Translation validation for the real C optimization passes.
 *
 * `rules.c` validates claimed rule pairs, which is not the same as validating
 * that a pass implements them: a pass could no-op and still agree with its
 * input. Each case here therefore requires four things at once:
 *
 *   1. the input verifies and evaluates;
 *   2. the pass reports that it changed the function;
 *   3. the output verifies and the targeted operation occurred fewer times;
 *   4. the output evaluates identically over the whole domain.
 *
 * Requirement 3 is what makes this translation validation rather than a
 * vacuous equality check: if a pass stops firing, the case fails instead of
 * passing because "nothing changed, nothing broke".
 */
#include "opt.h"

extern bool nyr_const_fold(nyr_func_t *f);
extern bool nyr_peephole(nyr_func_t *f);
extern bool nyr_egraph_rewrite(nyr_func_t *f);
extern bool nyr_cse(nyr_func_t *f);
extern bool nyr_copy_prop(nyr_func_t *f);

static const int64_t k_domain[] = {
    0,     1,     2,     3,     7,     8,      15,      16,
    31,    63,    127,   255,   1024,  -1,     -2,      -7,
    -256,  INT64_C(1) << 32,     INT64_MAX};

static const size_t k_domain_len = sizeof(k_domain) / sizeof(k_domain[0]);

/*
 * How to show the pass did the work it claims. Folding passes delete the
 * operation outright; propagation and the e-graph replace every use and leave
 * the node for later DCE, so "still present" is not the same as "still live".
 */
typedef enum {
  METRIC_COUNT,  /* the operation occurs fewer times */
  METRIC_UNUSED, /* every remaining instance of it is dead */
} pass_metric_t;

typedef struct {
  const char *name;
  void (*build)(nyr_func_t *f);
  bool (*pass)(nyr_func_t *f);
  nyr_op_t counted; /* operation the pass must reduce or kill */
  pass_metric_t metric;
} pass_case_t;

/*
 * All constants: constant folding must leave no ADD_I64 behind.
 */
static void build_fold_constants(nyr_func_t *f) {
  nyr_opt_begin(f);
  int inner = nyr_opt_binop(f, NYIR_ADD_I64, nyr_opt_const(f, 2),
                            nyr_opt_const(f, 3));
  int outer = nyr_opt_binop(f, NYIR_MUL_I64, inner, nyr_opt_const(f, 4));
  nyr_opt_ret(f, outer);
}

/*
 * x + 0: peephole removes the addition.
 */
static void build_add_zero(nyr_func_t *f) {
  nyr_opt_begin(f);
  int x = nyr_opt_emit_imm(f, NYIR_LOAD_LOCAL, 0);
  nyr_opt_ret(f, nyr_opt_binop(f, NYIR_ADD_I64, x, nyr_opt_const(f, 0)));
}

/*
 * x * 8: the e-graph rewrites the multiply into a shift.
 */
static void build_mul_pow2(nyr_func_t *f) {
  nyr_opt_begin(f);
  int x = nyr_opt_emit_imm(f, NYIR_LOAD_LOCAL, 0);
  nyr_opt_ret(f, nyr_opt_binop(f, NYIR_MUL_I64, x, nyr_opt_const(f, 8)));
}

/*
 * Two identical additions: CSE keeps one of them.
 */
static void build_redundant_adds(nyr_func_t *f) {
  nyr_opt_begin(f);
  int x = nyr_opt_emit_imm(f, NYIR_LOAD_LOCAL, 0);
  int one = nyr_opt_const(f, 1);
  int first = nyr_opt_binop(f, NYIR_ADD_I64, x, one);
  int second = nyr_opt_binop(f, NYIR_ADD_I64, x, one);
  nyr_opt_ret(f, nyr_opt_binop(f, NYIR_ADD_I64, first, second));
}

/*
 * t = copy(x); t + 1: copy propagation removes the COPY.
 */
static void build_copy(nyr_func_t *f) {
  nyr_opt_begin(f);
  int x = nyr_opt_emit_imm(f, NYIR_LOAD_LOCAL, 0);
  int copied = nyr_opt_emit_abc(f, NYIR_COPY, x, -1, -1);
  nyr_opt_ret(f, nyr_opt_binop(f, NYIR_ADD_I64, copied, nyr_opt_const(f, 1)));
}

static const pass_case_t k_cases[] = {
    {"const-fold", build_fold_constants, nyr_const_fold, NYIR_ADD_I64,
     METRIC_COUNT},
    {"peephole-add-zero", build_add_zero, nyr_peephole, NYIR_ADD_I64,
     METRIC_COUNT},
    {"egraph-mul-pow2", build_mul_pow2, nyr_egraph_rewrite, NYIR_MUL_I64,
     METRIC_UNUSED},
    {"cse-redundant-add", build_redundant_adds, nyr_cse, NYIR_ADD_I64,
     METRIC_COUNT},
    {"copy-prop", build_copy, nyr_copy_prop, NYIR_COPY, METRIC_UNUSED},
};

static size_t op_count(const nyr_func_t *f, nyr_op_t op) {
  size_t n = 0;
  for (size_t i = 0; i < f->len; ++i)
    if (f->data[i].op == op)
      ++n;
  return n;
}

/*
 * Number of live instances of `op`: an instance is live when its defined value
 * still has at least one user. The use-def index is the same one the passes
 * consume, so this asks exactly the question that matters for later code
 * generation rather than counting dead instructions.
 */
static size_t live_op_count(const nyr_func_t *f, nyr_op_t op) {
  nyr_use_def_t uses = {0};
  if (!nyr_build_use_def(f, &uses))
    return op_count(f, op);
  size_t live = 0;
  for (size_t i = 0; i < f->len; ++i) {
    if (f->data[i].op != op)
      continue;
    int dst = f->data[i].dst;
    bool used = dst >= 0 && (size_t)dst + 1 < uses.value_count &&
                uses.offsets[dst + 1] > uses.offsets[dst];
    if (used)
      ++live;
  }
  nyr_use_def_free(&uses);
  return live;
}

static bool eval_at(nyr_func_t *f, int64_t input, int64_t *out) {
  int64_t locals[8] = {input, 0, 0, 0, 0, 0, 0, 0};
  return nyr_opt_eval_locals(f, "passes", locals, 8, out);
}

static int failures;

static bool check_case(const pass_case_t *c) {
  nyr_func_t f;
  c->build(&f);
  if (!nyr_opt_verify(&f, c->name)) {
    fprintf(stderr, "[passes] %s: input does not verify\n", c->name);
    ++failures;
    nyr_ir_func_free(&f);
    return false;
  }
  size_t before = op_count(&f, c->counted);
  int64_t reference[64] = {0};
  if (k_domain_len > sizeof(reference) / sizeof(reference[0])) {
    fprintf(stderr, "[passes] %s: domain larger than reference buffer\n",
            c->name);
    ++failures;
    nyr_ir_func_free(&f);
    return false;
  }
  for (size_t i = 0; i < k_domain_len; ++i) {
    if (!eval_at(&f, k_domain[i], &reference[i])) {
      fprintf(stderr, "[passes] %s: input evaluation failed\n", c->name);
      ++failures;
      nyr_ir_func_free(&f);
      return false;
    }
  }
  bool changed = c->pass(&f);
  size_t after = op_count(&f, c->counted);
  size_t live_before = c->metric == METRIC_UNUSED ? before : 0;
  size_t live_after = c->metric == METRIC_UNUSED ? live_op_count(&f, c->counted)
                                                 : 0;
  bool effect = c->metric == METRIC_COUNT ? after < before
                                          : live_after < live_before;
  bool ok = changed && nyr_opt_verify(&f, c->name) && effect;
  for (size_t i = 0; ok && i < k_domain_len; ++i) {
    int64_t value = 0;
    if (!eval_at(&f, k_domain[i], &value) || value != reference[i]) {
      fprintf(stderr, "[passes] %s: x=%lld changed %lld -> %lld\n", c->name,
              (long long)k_domain[i], (long long)reference[i],
              (long long)value);
      ok = false;
    }
  }
  if (!ok) {
    fprintf(stderr,
            "[passes] %s: changed=%d op %d count=%zu->%zu live=%zu->%zu (the "
            "pass must fire, stay valid, and kill or reduce that operation)\n",
            c->name, (int)changed, (int)c->counted, before, after, live_before,
            live_after);
    nyr_dump(stderr, &f, c->name);
    ++failures;
  }
  nyr_ir_func_free(&f);
  return ok;
}

int main(void) {
  size_t passed = 0;
  for (size_t i = 0; i < sizeof(k_cases) / sizeof(k_cases[0]); ++i)
    if (check_case(&k_cases[i]))
      ++passed;
  if (failures) {
    fprintf(stderr, "[passes] %d pass case(s) failed validation\n", failures);
    return 1;
  }
  printf("[passes] %zu passes fired, stayed verifiable, reduced their target "
         "operation, and preserved %zu inputs\n",
         passed, k_domain_len);
  return 0;
}
