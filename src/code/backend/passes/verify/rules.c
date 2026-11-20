/*
 * Bounded exhaustive validation of scalar rewrite rules.
 *
 * Each rule is a pair of functions: the unrewritten form and the form the
 * optimizer claims is equivalent. Both are evaluated by the same reference
 * interpreter over a fixed domain of inputs, and every result must match. This
 * is the C counterpart of a proved rewrite: equality on the tested domain is
 * evidence, not a theorem, and the domain is deliberately small enough to
 * enumerate and large enough to contain every boundary of the 64-bit value
 * space that a rule is likely to get wrong.
 *
 * The last case is the opposite: it records a rule that is only valid under a
 * precondition (non-negative dividend), and fails if the unrestricted forms
 * ever start agreeing, so the guard cannot be deleted silently.
 */
#include "opt.h"

static const int64_t k_domain[] = {
    0,          1,          2,          3,          7,          8,
    15,         16,         31,         63,         127,        255,
    256,        1024,       -1,         -2,         -7,         -256,
    INT64_C(1) << 32,       INT64_MAX,  INT64_MIN};

static const size_t k_domain_len = sizeof(k_domain) / sizeof(k_domain[0]);

enum { MAX_RULE_FAILURES = 16 };

static int failures;

typedef struct {
  const char *name;
  void (*build)(nyr_func_t *f, bool rewritten);
} rule_t;

/*
 * Parameter x is local slot 0, so one build serves every domain value.
 * Rules that would trap (division by zero, oversized shifts) are constructed
 * with constant divisors and masked shift counts.
 */
static void rule_add_zero(nyr_func_t *f, bool rw) {
  nyr_opt_begin(f);
  int x = nyr_opt_emit_imm(f, NYIR_LOAD_LOCAL, 0);
  nyr_opt_ret(f, rw ? x : nyr_opt_binop(f, NYIR_ADD_I64, x, nyr_opt_const(f, 0)));
}

static void rule_mul_one(nyr_func_t *f, bool rw) {
  nyr_opt_begin(f);
  int x = nyr_opt_emit_imm(f, NYIR_LOAD_LOCAL, 0);
  nyr_opt_ret(f, rw ? x : nyr_opt_binop(f, NYIR_MUL_I64, x, nyr_opt_const(f, 1)));
}

static void rule_mul_pow2(nyr_func_t *f, bool rw) {
  nyr_opt_begin(f);
  int x = nyr_opt_emit_imm(f, NYIR_LOAD_LOCAL, 0);
  nyr_opt_ret(f, rw ? nyr_opt_binop(f, NYIR_SHL_I64, x, nyr_opt_const(f, 3))
                    : nyr_opt_binop(f, NYIR_MUL_I64, x, nyr_opt_const(f, 8)));
}

static void rule_sub_self(nyr_func_t *f, bool rw) {
  nyr_opt_begin(f);
  int x = nyr_opt_emit_imm(f, NYIR_LOAD_LOCAL, 0);
  nyr_opt_ret(f, rw ? nyr_opt_const(f, 0)
                    : nyr_opt_binop(f, NYIR_SUB_I64, x, x));
}

static void rule_and_self(nyr_func_t *f, bool rw) {
  nyr_opt_begin(f);
  int x = nyr_opt_emit_imm(f, NYIR_LOAD_LOCAL, 0);
  nyr_opt_ret(f, rw ? x : nyr_opt_binop(f, NYIR_AND_I64, x, x));
}

static void rule_or_self(nyr_func_t *f, bool rw) {
  nyr_opt_begin(f);
  int x = nyr_opt_emit_imm(f, NYIR_LOAD_LOCAL, 0);
  nyr_opt_ret(f, rw ? x : nyr_opt_binop(f, NYIR_OR_I64, x, x));
}

static void rule_xor_self(nyr_func_t *f, bool rw) {
  nyr_opt_begin(f);
  int x = nyr_opt_emit_imm(f, NYIR_LOAD_LOCAL, 0);
  nyr_opt_ret(f, rw ? nyr_opt_const(f, 0)
                    : nyr_opt_binop(f, NYIR_XOR_I64, x, x));
}

static void rule_shift_zero(nyr_func_t *f, bool rw) {
  nyr_opt_begin(f);
  int x = nyr_opt_emit_imm(f, NYIR_LOAD_LOCAL, 0);
  nyr_opt_ret(f, rw ? x : nyr_opt_binop(f, NYIR_SHL_I64, x, nyr_opt_const(f, 0)));
}

static void rule_div_one(nyr_func_t *f, bool rw) {
  nyr_opt_begin(f);
  int x = nyr_opt_emit_imm(f, NYIR_LOAD_LOCAL, 0);
  nyr_opt_ret(f, rw ? x : nyr_opt_binop(f, NYIR_DIV_I64, x, nyr_opt_const(f, 1)));
}

/*
 * (x & 0xff) & 0x0f  ==  x & 0x0f ; mask chains combine.
 */
static void rule_mask_combine(nyr_func_t *f, bool rw) {
  nyr_opt_begin(f);
  int x = nyr_opt_emit_imm(f, NYIR_LOAD_LOCAL, 0);
  if (rw) {
    nyr_opt_ret(f, nyr_opt_binop(f, NYIR_AND_I64, x, nyr_opt_const(f, 0x0f)));
    return;
  }
  int low = nyr_opt_binop(f, NYIR_AND_I64, x, nyr_opt_const(f, 0xff));
  nyr_opt_ret(f, nyr_opt_binop(f, NYIR_AND_I64, low, nyr_opt_const(f, 0x0f)));
}

/*
 * (x + 3) + 4  ==  x + 7 ; constant reassociation.
 */
static void rule_add_combine(nyr_func_t *f, bool rw) {
  nyr_opt_begin(f);
  int x = nyr_opt_emit_imm(f, NYIR_LOAD_LOCAL, 0);
  if (rw) {
    nyr_opt_ret(f, nyr_opt_binop(f, NYIR_ADD_I64, x, nyr_opt_const(f, 7)));
    return;
  }
  int a = nyr_opt_binop(f, NYIR_ADD_I64, x, nyr_opt_const(f, 3));
  nyr_opt_ret(f, nyr_opt_binop(f, NYIR_ADD_I64, a, nyr_opt_const(f, 4)));
}

/*
 * select(x < 0, 7, 9) is not constant; select(1, 7, 9) folds to 7.
 */
static void rule_select_const(nyr_func_t *f, bool rw) {
  nyr_opt_begin(f);
  int seven = nyr_opt_const(f, 7);
  int nine = nyr_opt_const(f, 9);
  if (rw) {
    nyr_opt_ret(f, seven);
    return;
  }
  int cond = nyr_opt_cmp(f, nyr_opt_const(f, 1), nyr_opt_const(f, 0),
                         NYIR_CMP_GT);
  nyr_opt_ret(f, nyr_opt_select(f, cond, seven, nine));
}

static const rule_t k_rules[] = {
    {"add-zero", rule_add_zero},      {"mul-one", rule_mul_one},
    {"mul-pow2", rule_mul_pow2},      {"sub-self", rule_sub_self},
    {"and-self", rule_and_self},      {"or-self", rule_or_self},
    {"xor-self", rule_xor_self},      {"shift-zero", rule_shift_zero},
    {"div-one", rule_div_one},        {"mask-combine", rule_mask_combine},
    {"add-combine", rule_add_combine}, {"select-const", rule_select_const},
};

static const size_t k_rule_count = sizeof(k_rules) / sizeof(k_rules[0]);

static bool eval_with_input(nyr_func_t *f, int64_t input, int64_t *out) {
  int64_t locals[8] = {input, 0, 0, 0, 0, 0, 0, 0};
  return nyr_opt_eval_locals(f, "rule", locals, 8, out);
}

static bool check_rule(const rule_t *rule) {
  bool ok = true;
  for (size_t i = 0; i < k_domain_len; ++i) {
    int64_t input = k_domain[i];
    nyr_func_t original;
    nyr_func_t rewritten;
    rule->build(&original, false);
    rule->build(&rewritten, true);
    int64_t before = 0, after = 0;
    bool good = nyr_opt_verify(&original, rule->name) &&
                nyr_opt_verify(&rewritten, rule->name) &&
                eval_with_input(&original, input, &before) &&
                eval_with_input(&rewritten, input, &after) && before == after;
    if (!good) {
      fprintf(stderr, "[rules] %s: x=%lld original=%lld rewritten=%lld\n",
              rule->name, (long long)input, (long long)before,
              (long long)after);
      ok = false;
      ++failures;
    }
    nyr_ir_func_free(&original);
    nyr_ir_func_free(&rewritten);
    if (failures >= MAX_RULE_FAILURES)
      return false;
  }
  return ok;
}

/*
 * x / 4  is  NOT  x >> 2 for negative x (C truncates toward zero, arithmetic
 * shift floors). The guarded rewrite is only valid for a non-negative
 * dividend, so the unrestricted pair must disagree somewhere; if it stops
 * disagreeing, the guard has been made meaningless and this case fails.
 */
static void record_precondition_counterexample(void) {
  nyr_func_t div_by_four;
  nyr_func_t shift_by_two;
  nyr_opt_begin(&div_by_four);
  int x = nyr_opt_emit_imm(&div_by_four, NYIR_LOAD_LOCAL, 0);
  nyr_opt_ret(&div_by_four,
              nyr_opt_binop(&div_by_four, NYIR_DIV_I64, x, nyr_opt_const(&div_by_four, 4)));
  nyr_opt_begin(&shift_by_two);
  x = nyr_opt_emit_imm(&shift_by_two, NYIR_LOAD_LOCAL, 0);
  nyr_opt_ret(&shift_by_two,
              nyr_opt_binop(&shift_by_two, NYIR_SAR_I64, x, nyr_opt_const(&shift_by_two, 2)));

  size_t disagreements = 0;
  for (size_t i = 0; i < k_domain_len; ++i) {
    int64_t before = 0, after = 0;
    if (!eval_with_input(&div_by_four, k_domain[i], &before) ||
        !eval_with_input(&shift_by_two, k_domain[i], &after)) {
      fprintf(stderr, "[rules] div/4 precondition probe failed to evaluate\n");
      ++failures;
      break;
    }
    if (before != after)
      ++disagreements;
  }
  nyr_ir_func_free(&div_by_four);
  nyr_ir_func_free(&shift_by_two);
  if (disagreements == 0) {
    fprintf(stderr,
            "[rules] div/4 and sar/2 agreed everywhere; the non-negative "
            "guard no longer has a counterexample\n");
    ++failures;
    return;
  }
  printf("[rules] div/4 vs sar/2 disagrees on %zu domain values (the guard is "
         "load-bearing)\n",
         disagreements);
}

int main(void) {
  size_t checked = 0;
  for (size_t i = 0; i < k_rule_count; ++i)
    if (check_rule(&k_rules[i]))
      ++checked;
  record_precondition_counterexample();
  if (failures) {
    fprintf(stderr, "[rules] %d mismatch(es) over %zu domain values\n", failures,
            k_domain_len);
    return 1;
  }
  printf("[rules] %zu rules x %zu inputs: original and rewritten IR agree\n",
         checked, k_domain_len);
  return 0;
}
