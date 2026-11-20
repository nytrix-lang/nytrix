/*
 * Fault-injection suite for NYIR structural validation.
 *
 * A verifier that accepts an invalid function is worse than no verifier: the
 * defect survives to machine code and becomes a miscompile. Each case builds a
 * function the verifier must accept, corrupts exactly one invariant, and
 * requires the verifier to reject it. Controls require acceptance, so a
 * verifier that simply rejects everything cannot pass this suite.
 *
 * Covered invariants: defined operands, def-before-use, single SSA definition,
 * existing branch targets, PHI predecessor edges, a terminator on every path,
 * operand type agreement, and dominance of every use.
 */
#include "opt.h"

static int failures;

static bool expect_accept(const char *what, const nyr_func_t *f, bool want) {
  char err[256] = {0};
  bool accepted = nyr_ir_verify(f, err, sizeof(err));
  if (accepted == want)
    return true;
  fprintf(stderr, "[invariants] %s: verifier %s, expected %s%s%s\n", what,
          accepted ? "accepted" : "rejected", want ? "accept" : "reject",
          err[0] ? "; said: " : "", err);
  ++failures;
  return false;
}

/*
 * x = local0; return (x + 1) * 2  -- the control for the straight-line cases.
 */
static void build_chain(nyr_func_t *f) {
  nyr_opt_begin(f);
  int x = nyr_opt_emit_imm(f, NYIR_LOAD_LOCAL, 0);
  int one = nyr_opt_const(f, 1);
  int two = nyr_opt_const(f, 2);
  int sum = nyr_opt_binop(f, NYIR_ADD_I64, x, one);
  int scaled = nyr_opt_binop(f, NYIR_MUL_I64, sum, two);
  nyr_opt_ret(f, scaled);
}

/*
 * Local index of the ADD inside build_chain().
 */
enum { CHAIN_ADD = 3, CHAIN_MUL = 4, CHAIN_RET = 5 };

static void case_control(void) {
  nyr_func_t f;
  build_chain(&f);
  expect_accept("control chain", &f, true);
  /*
   * The same function must also evaluate, so the control is not vacuous.
   */
  int64_t locals[8] = {5};
  int64_t value = 0;
  if (!nyr_opt_eval_locals(&f, "control", locals, 8, &value) || value != 12) {
    fprintf(stderr, "[invariants] control evaluated to %lld, want 12\n",
            (long long)value);
    ++failures;
  }
  nyr_ir_func_free(&f);
}

static void case_undefined_operand(void) {
  nyr_func_t f;
  build_chain(&f);
  f.data[CHAIN_ADD].a = 999; /* no value 999 exists */
  expect_accept("undefined operand", &f, false);
  nyr_ir_func_free(&f);
}

static void case_use_before_def(void) {
  nyr_func_t f;
  build_chain(&f);
  f.data[CHAIN_ADD].a = f.data[CHAIN_MUL].dst; /* forward reference */
  expect_accept("use before definition", &f, false);
  nyr_ir_func_free(&f);
}

static void case_duplicate_definition(void) {
  nyr_func_t f;
  build_chain(&f);
  f.data[CHAIN_MUL].dst = f.data[CHAIN_ADD].dst;
  expect_accept("duplicate SSA definition", &f, false);
  nyr_ir_func_free(&f);
}

static void case_missing_branch_target(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int x = nyr_opt_emit_imm(&f, NYIR_LOAD_LOCAL, 0);
  nyr_opt_br_if(&f, x, 10);
  nyr_opt_br(&f, 7); /* label 7 is never defined */
  nyr_opt_label(&f, 10);
  nyr_opt_ret(&f, x);
  expect_accept("missing branch target", &f, false);
  nyr_ir_func_free(&f);
}

static void case_missing_terminator(void) {
  nyr_func_t f;
  build_chain(&f);
  f.data[CHAIN_RET].op = NYIR_NOP; /* fall off the end of the function */
  expect_accept("missing terminator", &f, false);
  nyr_ir_func_free(&f);
}

static void case_type_mismatch(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int i = nyr_opt_const(&f, 1);
  int sum = nyr_opt_binop(&f, NYIR_ADD_F64, i, i); /* f64 add of i64 values */
  nyr_opt_ret(&f, sum);
  expect_accept("float op on integer operands", &f, false);
  nyr_ir_func_free(&f);
}

/*
 * A value defined only on the L10 arm is used at the join: the entry -> L20 ->
 * L30 path never executes its definition, so the use is not dominated.
 */
static void case_non_dominating_use(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int x = nyr_opt_emit_imm(&f, NYIR_LOAD_LOCAL, 0);
  nyr_opt_br_if(&f, x, 10);
  nyr_opt_br(&f, 20);
  nyr_opt_label(&f, 10);
  int arm = nyr_opt_binop(&f, NYIR_ADD_I64, x, nyr_opt_const(&f, 1));
  nyr_opt_br(&f, 30);
  nyr_opt_label(&f, 20);
  nyr_opt_br(&f, 30);
  nyr_opt_label(&f, 30);
  nyr_opt_ret(&f, arm);
  expect_accept("non-dominating use", &f, false);
  nyr_ir_func_free(&f);
}

/*
 * Valid diamond: both arms define a value, joined by a two-edge PHI.
 */
static size_t build_diamond(nyr_func_t *f) {
  nyr_opt_begin(f);
  int x = nyr_opt_emit_imm(f, NYIR_LOAD_LOCAL, 0);
  nyr_opt_br_if(f, x, 10);
  nyr_opt_br(f, 20);
  nyr_opt_label(f, 10);
  int lhs = nyr_opt_binop(f, NYIR_ADD_I64, x, nyr_opt_const(f, 1));
  nyr_opt_br(f, 30);
  nyr_opt_label(f, 20);
  int rhs = nyr_opt_binop(f, NYIR_SUB_I64, x, nyr_opt_const(f, 1));
  nyr_opt_br(f, 30);
  nyr_opt_label(f, 30);
  nyr_opt_phi(f, 10, lhs, 20, rhs);
  size_t phi_index = f->len - 1;
  nyr_opt_ret(f, f->data[phi_index].dst);
  return phi_index;
}

static void case_phi_edge_mismatch(void) {
  nyr_func_t f;
  size_t phi = build_diamond(&f);
  expect_accept("control diamond", &f, true);
  f.data[phi].phi_incoming[0].predecessor_label = 99; /* not a predecessor */
  expect_accept("PHI edge to a non-predecessor", &f, false);
  nyr_ir_func_free(&f);
}

static void case_phi_duplicate_edge(void) {
  nyr_func_t f;
  size_t phi = build_diamond(&f);
  f.data[phi].phi_incoming[1].predecessor_label =
      f.data[phi].phi_incoming[0].predecessor_label;
  expect_accept("PHI with a duplicate predecessor", &f, false);
  nyr_ir_func_free(&f);
}

static void case_nested_select_condition(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int x = nyr_opt_emit_imm(&f, NYIR_LOAD_LOCAL, 0);
  int zero = nyr_opt_const(&f, 0);
  int one = nyr_opt_const(&f, 1);
  int two = nyr_opt_const(&f, 2);
  int positive = nyr_opt_cmp(&f, zero, x, NYIR_CMP_LT);
  int is_one = nyr_opt_cmp(&f, x, one, NYIR_CMP_EQ);
  size_t inner = f.len;
  int condition = nyr_opt_select(&f, positive, is_one, zero);
  nyr_opt_ret(&f, nyr_opt_select(&f, condition, x, two));
  expect_accept("select with two boolean arms as a condition", &f, true);
  f.data[inner].c = two;
  expect_accept("select condition with a non-boolean false arm", &f, false);
  f.data[inner].c = zero;
  f.data[inner].b = two;
  expect_accept("select condition with a non-boolean true arm", &f, false);
  nyr_ir_func_free(&f);
}

int main(void) {
  case_control();
  case_undefined_operand();
  case_use_before_def();
  case_duplicate_definition();
  case_missing_branch_target();
  case_missing_terminator();
  case_type_mismatch();
  case_non_dominating_use();
  case_phi_edge_mismatch();
  case_phi_duplicate_edge();
  case_nested_select_condition();
  if (failures) {
    fprintf(stderr, "[invariants] %d injected defect(s) escaped validation\n",
            failures);
    return 1;
  }
  printf("[invariants] controls accepted, every injected "
         "defect rejected\n");
  return 0;
}
