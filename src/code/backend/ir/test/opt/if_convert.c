/*
 * Exact-diamond SELECT regression: semantic equivalence and safety gate.
 */
#include "code/lang/parse/ast.h"
#include "support.h"

static void build_diamond(nyr_func_t *f, int64_t right, bool side_effect,
                          bool bitmask_condition, bool true_first_block) {
  nyr_opt_begin(f);
  int left = nyr_opt_const(f, 4);
  int rhs = nyr_opt_const(f, right);
  int one = nyr_opt_const(f, 1);
  int two = nyr_opt_const(f, 2);
  int cond = bitmask_condition ? nyr_opt_binop(f, NYIR_AND_I64, left, one)
                               : nyr_opt_cmp(f, left, rhs, NYIR_CMP_EQ);
  nyr_opt_br_if(f, cond, 20);
  if (true_first_block) {
    nyr_opt_label(f, 15);
    nyr_opt_br(f, 10);
  }
  int false_value = -1, true_value = -1;
  for (int arm = 0; arm < 2; ++arm) {
    bool true_arm = true_first_block ? arm == 0 : arm == 1;
    nyr_opt_label(f, true_arm ? 20 : 10);
    int value = nyr_opt_binop(f, NYIR_ADD_I64, left, true_arm ? two : one);
    if (!true_arm && side_effect)
      nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_STORE_LOCAL,
                                  .dst = -1,
                                  .a = value,
                                  .b = -1,
                                  .c = -1,
                                  .d = -1,
                                  .e = -1,
                                  .f = -1,
                                  .imm = 0});
    if (true_arm)
      true_value = value;
    else
      false_value = value;
    nyr_opt_br(f, 30);
  }
  nyr_opt_label(f, 30);
  int result = f->next_value;
  nyr_opt_phi(f, 10, false_value, 20, true_value);
  nyr_opt_ret(f, result);
}

static size_t count_op(const nyr_func_t *f, nyr_op_t op) {
  size_t count = 0;
  for (size_t i = 0; i < f->len; ++i)
    count += f->data[i].op == op;
  return count;
}

static int run_phi_metadata_case(void) {
  static const char source[] = "if-convert-phi-origin.ny";
  const char *name = "if-convert-phi-metadata";
  nyr_func_t f;
  build_diamond(&f, 4, false, false, false);
  size_t phi_at = SIZE_MAX;
  for (size_t i = 0; i < f.len; ++i)
    if (f.data[i].op == NYIR_PHI) {
      phi_at = i;
      break;
    }
  if (phi_at == SIZE_MAX) {
    nyr_ir_func_free(&f);
    return 1;
  }
  const int result = f.data[phi_at].dst;
  f.data[phi_at].scalar_class = NYIR_SCALAR_I64_RAW;
  f.data[phi_at].semantic_ownership = NY_SEM_OWN_OWNED;
  f.data[phi_at].semantic_mutable = true;
  f.data[phi_at].range =
      (nyr_range_t){.has_min = true, .has_max = true, .min = 5, .max = 6};
  f.data[phi_at].debug =
      (nyr_debug_loc_t){.file = source, .line = 12, .column = 4};

  bool ok = nyr_opt_verify(&f, name) && nyr_opt_eval(&f, name) == 6 &&
            nyr_if_convert(&f) && nyr_opt_verify(&f, name) &&
            nyr_opt_eval(&f, name) == 6;
  bool found = false;
  for (size_t i = 0; ok && i < f.len; ++i) {
    const nyr_inst_t *in = &f.data[i];
    if (in->dst != result)
      continue;
    found = true;
    ok = in->op == NYIR_SELECT_I64 && in->scalar_class == NYIR_SCALAR_I64_RAW &&
         in->semantic_ownership == NY_SEM_OWN_OWNED && in->semantic_mutable &&
         in->range.has_min && in->range.has_max && in->range.min == 5 &&
         in->range.max == 6 && in->debug.file == source &&
         in->debug.line == 12 && in->debug.column == 4;
  }
  ok = ok && found;
  if (!ok)
    nyr_dump(stderr, &f, name);
  nyr_ir_func_free(&f);
  if (ok)
    puts("[if-convert] PHI metadata preserved on SELECT");
  return ok ? 0 : 1;
}

static int run_case(const char *name, int64_t right, bool side_effect,
                    bool bitmask_condition, bool true_first_block,
                    long long expected, bool converted) {
  nyr_func_t f;
  build_diamond(&f, right, side_effect, bitmask_condition, true_first_block);
  if (!nyr_opt_verify(&f, name))
    return 1;
  long long before = nyr_opt_eval(&f, name);
  if (!nyr_if_convert(&f) || !nyr_opt_verify(&f, name)) {
    nyr_ir_func_free(&f);
    return 1;
  }
  long long after = nyr_opt_eval(&f, name);
  size_t selects = count_op(&f, NYIR_SELECT_I64);
  size_t phis = count_op(&f, NYIR_PHI);
  if ((converted && (selects != 1 || phis != 0)) ||
      (!converted && (selects != 0 || phis != 1))) {
    fprintf(stderr, "[%s] unexpected conversion select=%zu phi=%zu\n", name,
            selects, phis);
    nyr_dump(stderr, &f, name);
    nyr_ir_func_free(&f);
    return 1;
  }
  nyr_ir_func_free(&f);
  nyr_opt_check(name, before, after, expected);
  return 0;
}

static int run_forward_trampoline_case(const char *name, int64_t input,
                                       long long expected, bool named_bridge) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int value = nyr_opt_const(&f, input);
  int zero = nyr_opt_const(&f, 0);
  int condition = nyr_opt_cmp(&f, value, zero, NYIR_CMP_NE);
  int one = nyr_opt_const(&f, 1), two = nyr_opt_const(&f, 2);
  int three = nyr_opt_const(&f, 3);
  nyr_opt_br_if(&f, condition, 20);
  if (named_bridge)
    nyr_opt_label(&f, 15);
  nyr_opt_br(&f, 10); /* empty false-edge bridge emitted by source lowering */
  nyr_opt_label(&f, 20);
  int true_value = nyr_opt_binop(&f, NYIR_ADD_I64, value, one);
  true_value = nyr_opt_binop(&f, NYIR_ADD_I64, true_value, two);
  nyr_opt_br(&f, 30);
  nyr_opt_label(&f, 10);
  int false_value = nyr_opt_binop(&f, NYIR_ADD_I64, value, two);
  false_value = nyr_opt_binop(&f, NYIR_ADD_I64, false_value, three);
  /*
   * The false arm falls through directly into the adjacent join block.
   */
  nyr_opt_label(&f, 30);
  int result = f.next_value;
  nyr_opt_phi(&f, 10, false_value, 20, true_value);
  nyr_opt_ret(&f, result);

  if (!nyr_opt_verify(&f, name)) {
    nyr_ir_func_free(&f);
    return 1;
  }
  long long before = nyr_opt_eval(&f, name);
  if (!nyr_if_convert(&f) || !nyr_opt_verify(&f, name)) {
    nyr_ir_func_free(&f);
    return 1;
  }
  long long after = nyr_opt_eval(&f, name);
  size_t selects = count_op(&f, NYIR_SELECT_I64);
  size_t phis = count_op(&f, NYIR_PHI);
  if (selects != 1 || phis != 0) {
    fprintf(stderr, "[%s] expected trampoline conversion select=%zu phi=%zu\n",
            name, selects, phis);
    nyr_ir_func_free(&f);
    return 1;
  }
  nyr_ir_func_free(&f);
  nyr_opt_check(name, before, after, expected);
  return 0;
}

static int run_speculation_cost_case(void) {
  const char *name = "if-convert-speculation-cost";
  nyr_func_t f;
  nyr_opt_begin(&f);
  int value = nyr_opt_const(&f, 3), zero = nyr_opt_const(&f, 0);
  int one = nyr_opt_const(&f, 1), two = nyr_opt_const(&f, 2),
      three = nyr_opt_const(&f, 3);
  int four = nyr_opt_const(&f, 4), five = nyr_opt_const(&f, 5),
      six = nyr_opt_const(&f, 6);
  int condition = nyr_opt_cmp(&f, value, zero, NYIR_CMP_NE);
  nyr_opt_br_if(&f, condition, 20);
  nyr_opt_label(&f, 10);
  int false_value = nyr_opt_binop(&f, NYIR_ADD_I64, value, four);
  false_value = nyr_opt_binop(&f, NYIR_ADD_I64, false_value, five);
  false_value = nyr_opt_binop(&f, NYIR_ADD_I64, false_value, six);
  nyr_opt_br(&f, 30);
  nyr_opt_label(&f, 20);
  int true_value = nyr_opt_binop(&f, NYIR_ADD_I64, value, one);
  true_value = nyr_opt_binop(&f, NYIR_ADD_I64, true_value, two);
  true_value = nyr_opt_binop(&f, NYIR_ADD_I64, true_value, three);
  nyr_opt_br(&f, 30);
  nyr_opt_label(&f, 30);
  int result = f.next_value;
  nyr_opt_phi(&f, 10, false_value, 20, true_value);
  nyr_opt_ret(&f, result);
  if (!nyr_opt_verify(&f, name)) {
    nyr_ir_func_free(&f);
    return 1;
  }
  long long before = nyr_opt_eval(&f, name);
  if (!nyr_if_convert(&f) || !nyr_opt_verify(&f, name)) {
    nyr_ir_func_free(&f);
    return 1;
  }
  long long after = nyr_opt_eval(&f, name);
  size_t selects = count_op(&f, NYIR_SELECT_I64);
  size_t phis = count_op(&f, NYIR_PHI);
  nyr_ir_func_free(&f);
  nyr_opt_check(name, before, after, 9);
  if (selects != 0 || phis != 1) {
    fprintf(stderr,
            "[%s] costly arms must retain the branch (select=%zu phi=%zu)\n",
            name, selects, phis);
    return 1;
  }
  return 0;
}

static int run_multiply_speculation_cost_case(void) {
  const char *name = "if-convert-multiply-speculation-cost";
  nyr_func_t f;
  nyr_opt_begin(&f);
  int value = nyr_opt_const(&f, 3), zero = nyr_opt_const(&f, 0);
  int one = nyr_opt_const(&f, 1), two = nyr_opt_const(&f, 2);
  int four = nyr_opt_const(&f, 4), five = nyr_opt_const(&f, 5);
  int condition = nyr_opt_cmp(&f, value, zero, NYIR_CMP_NE);
  nyr_opt_br_if(&f, condition, 20);
  nyr_opt_label(&f, 10);
  int false_value = nyr_opt_binop(&f, NYIR_MUL_I64, value, four);
  false_value = nyr_opt_binop(&f, NYIR_ADD_I64, false_value, five);
  nyr_opt_br(&f, 30);
  nyr_opt_label(&f, 20);
  int true_value = nyr_opt_binop(&f, NYIR_MUL_I64, value, two);
  true_value = nyr_opt_binop(&f, NYIR_ADD_I64, true_value, one);
  nyr_opt_br(&f, 30);
  nyr_opt_label(&f, 30);
  int result = f.next_value;
  nyr_opt_phi(&f, 10, false_value, 20, true_value);
  nyr_opt_ret(&f, result);
  if (!nyr_opt_verify(&f, name)) {
    nyr_ir_func_free(&f);
    return 1;
  }
  long long before = nyr_opt_eval(&f, name);
  if (!nyr_if_convert(&f) || !nyr_opt_verify(&f, name)) {
    nyr_ir_func_free(&f);
    return 1;
  }
  long long after = nyr_opt_eval(&f, name);
  size_t selects = count_op(&f, NYIR_SELECT_I64);
  size_t phis = count_op(&f, NYIR_PHI);
  nyr_ir_func_free(&f);
  nyr_opt_check(name, before, after, 7);
  if (selects != 0 || phis != 1) {
    fprintf(stderr,
            "[%s] duplicated multiplies should retain the branch "
            "(select=%zu phi=%zu)\n",
            name, selects, phis);
    return 1;
  }
  return 0;
}

static int run_loop_speculation_cost_case(void) {
  const char *name = "if-convert-loop-vectorization-budget";
  nyr_func_t f;
  nyr_opt_begin(&f);
  int init = nyr_opt_const(&f, 0), trip = nyr_opt_const(&f, 2);
  int one = nyr_opt_const(&f, 1), zero = nyr_opt_const(&f, 0);
  int base = nyr_opt_const(&f, 2);
  int f1 = nyr_opt_const(&f, 4), f2 = nyr_opt_const(&f, 5),
      f3 = nyr_opt_const(&f, 6);
  int t1 = nyr_opt_const(&f, 1), t2 = nyr_opt_const(&f, 2),
      t3 = nyr_opt_const(&f, 3);
  nyr_opt_br(&f, 10);
  nyr_opt_label(&f, 10);
  int iv = f.next_value;
  size_t iv_phi_index = f.len;
  nyr_opt_phi(&f, -1, init, 50, -1);
  int accum = f.next_value;
  size_t accum_phi_index = f.len;
  nyr_opt_phi(&f, -1, zero, 50, -1);
  int truth = nyr_opt_cmp(&f, iv, zero, NYIR_CMP_EQ);
  int more = nyr_opt_cmp(&f, iv, trip, NYIR_CMP_LT);
  nyr_opt_br_if(&f, more, 20);
  nyr_opt_br(&f, 60);
  nyr_opt_label(&f, 20);
  nyr_opt_br_if(&f, truth, 30);
  nyr_opt_br(&f, 40);
  nyr_opt_label(&f, 30);
  int tv = nyr_opt_binop(&f, NYIR_MUL_I64, base, t1);
  tv = nyr_opt_binop(&f, NYIR_ADD_I64, tv, t3);
  tv = nyr_opt_binop(&f, NYIR_ADD_I64, tv, t2);
  nyr_opt_br(&f, 50);
  nyr_opt_label(&f, 40);
  int fv = nyr_opt_binop(&f, NYIR_MUL_I64, base, f1);
  fv = nyr_opt_binop(&f, NYIR_ADD_I64, fv, f3);
  fv = nyr_opt_binop(&f, NYIR_ADD_I64, fv, f2);
  nyr_opt_br(&f, 50);
  nyr_opt_label(&f, 50);
  int picked = f.next_value;
  nyr_opt_phi(&f, 40, fv, 30, tv);
  int next_accum = nyr_opt_binop(&f, NYIR_ADD_I64, accum, picked);
  int next_iv = nyr_opt_binop(&f, NYIR_ADD_I64, iv, one);
  /*
   * Fill the loop-header backedge values after both definitions exist.
   */
  f.data[iv_phi_index].phi_incoming[1].value = next_iv;
  f.data[accum_phi_index].phi_incoming[1].value = next_accum;
  nyr_opt_br(&f, 10);
  nyr_opt_label(&f, 60);
  nyr_opt_ret(&f, accum);

  if (!nyr_opt_verify(&f, name)) {
    nyr_ir_func_free(&f);
    return 1;
  }
  long long before = nyr_opt_eval(&f, name);
  if (!nyr_if_convert(&f) || !nyr_opt_verify(&f, name)) {
    nyr_ir_func_free(&f);
    return 1;
  }
  long long after = nyr_opt_eval(&f, name);
  size_t selects = count_op(&f, NYIR_SELECT_I64);
  nyr_ir_func_free(&f);
  nyr_opt_check(name, before, after, 26);
  if (selects != 1) {
    fprintf(
        stderr,
        "[%s] loop-local cost-10 diamond should become a select (got %zu)\n",
        name, selects);
    return 1;
  }
  return 0;
}

static int run_raw_i64_condition_case(void) {
  const char *name = "if-convert-raw-i64-condition";
  nyr_func_t f;
  build_diamond(&f, 7, false, false, false);
  f.data[4] = (nyr_inst_t){.op = NYIR_CONST_I64,
                           .dst = 4,
                           .a = -1,
                           .b = -1,
                           .c = -1,
                           .d = -1,
                           .e = -1,
                           .f = -1,
                           .imm = 2,
                           .scalar_class = NYIR_SCALAR_I64_RAW};
  if (!nyr_opt_verify(&f, name)) {
    nyr_ir_func_free(&f);
    return 1;
  }
  long long before = nyr_opt_eval(&f, name);
  if (!nyr_if_convert(&f) || !nyr_opt_verify(&f, name)) {
    nyr_ir_func_free(&f);
    return 1;
  }
  long long after = nyr_opt_eval(&f, name);
  size_t selects = count_op(&f, NYIR_SELECT_I64);
  nyr_ir_func_free(&f);
  if (selects != 1) {
    fprintf(stderr, "[%s] expected explicit truthiness compare and select\n",
            name);
    return 1;
  }
  nyr_opt_check(name, before, after, 6);
  return 0;
}

int main(void) {
  return run_phi_metadata_case() ||
         run_case("if-convert-false", 7, false, false, false, 5, true) ||
         run_case("if-convert-true", 4, false, false, false, 6, true) ||
         run_case("if-convert-true-first-block", 7, false, false, true, 5,
                  true) ||
         run_case("if-convert-bitmask-condition", 7, false, true, false, 5,
                  false) ||
         run_case("if-convert-side-effect", 7, true, false, false, 5, false) ||
         run_raw_i64_condition_case() ||
         run_forward_trampoline_case("if-convert-trampoline-false", 0, 5, true) ||
         run_forward_trampoline_case("if-convert-trampoline-true", 3, 6, true) ||
         run_forward_trampoline_case("if-convert-unlabelled-false", 0, 5, false) ||
         run_forward_trampoline_case("if-convert-unlabelled-true", 3, 6, false) ||
         run_speculation_cost_case() || run_multiply_speculation_cost_case() ||
         run_loop_speculation_cost_case();
}
