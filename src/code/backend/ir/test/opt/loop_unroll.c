/*
 * A PHI after an unrelated branch must not block local loop unroll.
 */
#include "base/debug.h"
#include "support.h"

extern bool nyr_loop_unroll(nyr_func_t *f);
extern bool nyr_loop_unroll_ssa(nyr_func_t *f);

static void store_local(nyr_func_t *f, int value, int64_t slot) {
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

static int load_local(nyr_func_t *f, int64_t slot) {
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

static void build_loop_with_unrelated_phi(nyr_func_t *f) {
  nyr_opt_begin(f);
  int zero = nyr_opt_const(f, 0);
  int one = nyr_opt_const(f, 1);
  int trip = nyr_opt_const(f, 4);
  int take_left = nyr_opt_const(f, 1);
  store_local(f, zero, 0);
  store_local(f, zero, 1);
  nyr_opt_br(f, 10);
  nyr_opt_label(f, 10);
  int iv = load_local(f, 0);
  int cond = nyr_opt_cmp(f, iv, trip, NYIR_CMP_LT);
  nyr_opt_br_if(f, cond, 11);
  nyr_opt_br(f, 30);
  nyr_opt_label(f, 11);
  int old_iv = load_local(f, 0);
  int next_iv = nyr_opt_binop(f, NYIR_ADD_I64, old_iv, one);
  store_local(f, next_iv, 0);
  int old_sum = load_local(f, 1);
  int next_sum = nyr_opt_binop(f, NYIR_ADD_I64, old_sum, one);
  store_local(f, next_sum, 1);
  nyr_opt_br(f, 10);
  /*
   * This post-loop diamond has a PHI, but is outside the unrolled loop.
   */
  nyr_opt_label(f, 30);
  nyr_opt_br_if(f, take_left, 31);
  nyr_opt_br(f, 32);
  nyr_opt_label(f, 31);
  nyr_opt_br(f, 33);
  nyr_opt_label(f, 32);
  nyr_opt_br(f, 33);
  nyr_opt_label(f, 33);
  int unrelated = f->next_value;
  nyr_opt_phi(f, 31, zero, 32, one);
  int sum = load_local(f, 1);
  int result = nyr_opt_binop(f, NYIR_ADD_I64, sum, unrelated);
  nyr_opt_ret(f, result);
}

static void build_loop_with_header_phi(nyr_func_t *f) {
  nyr_opt_begin(f);
  int zero = nyr_opt_const(f, 0);
  int one = nyr_opt_const(f, 1);
  int trip = nyr_opt_const(f, 4);
  nyr_opt_br(f, 10);
  nyr_opt_label(f, 10);
  size_t phi_at = f->len;
  int iv = f->next_value;
  nyr_opt_phi(f, -1, zero, 11, zero);
  int cond = nyr_opt_cmp(f, iv, trip, NYIR_CMP_LT);
  nyr_opt_br_if(f, cond, 11);
  nyr_opt_br(f, 20);
  nyr_opt_label(f, 11);
  int next_iv = nyr_opt_binop(f, NYIR_ADD_I64, iv, one);
  nyr_opt_br(f, 10);
  nyr_opt_label(f, 20);
  nyr_opt_ret(f, iv);
  f->data[phi_at].phi_incoming[1].value = next_iv;
}

static void build_ssa_counted_sum(nyr_func_t *f, int64_t trip,
                                  nyr_cmp_t predicate) {
  nyr_opt_begin(f);
  int zero = nyr_opt_const(f, 0);
  int one = nyr_opt_const(f, 1);
  int limit = nyr_opt_const(f, trip);
  int initial = nyr_opt_const(f, 10);
  nyr_opt_br(f, 10);
  nyr_opt_label(f, 10);
  size_t iv_phi = f->len;
  int iv = f->next_value;
  nyr_opt_phi(f, -1, zero, 11, zero);
  size_t sum_phi = f->len;
  int sum = f->next_value;
  nyr_opt_phi(f, -1, initial, 11, initial);
  int cond = nyr_opt_cmp(f, iv, limit, predicate);
  nyr_opt_br_if(f, cond, 11);
  nyr_opt_br(f, 20);
  nyr_opt_label(f, 20);
  nyr_opt_ret(f, sum);
  nyr_opt_label(f, 11);
  int next_sum = nyr_opt_binop(f, NYIR_ADD_I64, sum, iv);
  int next_iv = nyr_opt_binop(f, NYIR_ADD_I64, iv, one);
  nyr_opt_br(f, 10);
  f->data[iv_phi].phi_incoming[1].value = next_iv;
  f->data[sum_phi].phi_incoming[1].value = next_sum;
}

static void build_ssa_strided_sum(nyr_func_t *f) {
  nyr_opt_begin(f);
  int start = nyr_opt_const(f, 1);
  int limit = nyr_opt_const(f, 9);
  int stride = nyr_opt_const(f, 2);
  int initial = nyr_opt_const(f, 0);
  nyr_opt_br(f, 10);
  nyr_opt_label(f, 10);
  size_t iv_phi = f->len;
  int iv = f->next_value;
  nyr_opt_phi(f, -1, start, 11, start);
  size_t sum_phi = f->len;
  int sum = f->next_value;
  nyr_opt_phi(f, -1, initial, 11, initial);
  int cond = nyr_opt_cmp(f, iv, limit, NYIR_CMP_LT);
  nyr_opt_br_if(f, cond, 11);
  nyr_opt_br(f, 20);
  nyr_opt_label(f, 20);
  nyr_opt_ret(f, sum);
  nyr_opt_label(f, 11);
  int next_sum = nyr_opt_binop(f, NYIR_ADD_I64, sum, iv);
  int next_iv = nyr_opt_binop(f, NYIR_ADD_I64, iv, stride);
  nyr_opt_br(f, 10);
  f->data[iv_phi].phi_incoming[1].value = next_iv;
  f->data[sum_phi].phi_incoming[1].value = next_sum;
}

/*
 * A counted loop with a branch diamond and a nonmember block between its
 * header and body in lexical order.
 */
static void build_ssa_multiblock_branch_sum(nyr_func_t *f, bool divide_by_two) {
  nyr_opt_begin(f);
  int zero = nyr_opt_const(f, 0);
  int one = nyr_opt_const(f, 1);
  int trip = nyr_opt_const(f, 4);
  int ten = nyr_opt_const(f, 10);
  int twenty = nyr_opt_const(f, 20);
  nyr_opt_br(f, 10);

  nyr_opt_label(f, 10);
  size_t iv_phi = f->len;
  int iv = f->next_value;
  nyr_opt_phi(f, -1, zero, 14, zero);
  size_t sum_phi = f->len;
  int acc = f->next_value;
  nyr_opt_phi(f, -1, zero, 14, zero);
  int cond = nyr_opt_cmp(f, iv, trip, NYIR_CMP_LT);
  nyr_opt_br_if(f, cond, 11);
  nyr_opt_label(f, 20);
  nyr_opt_ret(f, acc);

  nyr_opt_label(f, 11);
  int parity = nyr_opt_binop(f, NYIR_AND_I64, iv, one);
  int is_even = nyr_opt_cmp(f, parity, zero, NYIR_CMP_EQ);
  nyr_opt_br_if(f, is_even, 13);
  nyr_opt_label(f, 12);
  int even_value = nyr_opt_binop(f, NYIR_ADD_I64, iv, ten);
  nyr_opt_br(f, 14);
  nyr_opt_label(f, 13);
  int odd_value = nyr_opt_binop(f, NYIR_ADD_I64, iv, twenty);
  nyr_opt_br(f, 14);
  nyr_opt_label(f, 14);
  int branch_value = f->next_value;
  nyr_opt_phi(f, 12, even_value, 13, odd_value);
  int contribution = branch_value;
  if (divide_by_two) {
    int two = nyr_opt_const(f, 2);
    contribution = nyr_opt_binop(f, NYIR_DIV_I64, branch_value, two);
  }
  int next_sum = nyr_opt_binop(f, NYIR_ADD_I64, acc, contribution);
  int next_iv = nyr_opt_binop(f, NYIR_ADD_I64, iv, one);
  nyr_opt_br(f, 10);

  f->data[iv_phi].phi_incoming[1].value = next_iv;
  f->data[sum_phi].phi_incoming[1].value = next_sum;
}

static void build_ssa_induction_division(nyr_func_t *f, int64_t start) {
  nyr_opt_begin(f);
  int zero = nyr_opt_const(f, 0);
  int one = nyr_opt_const(f, 1);
  int initial = nyr_opt_const(f, start);
  int limit = nyr_opt_const(f, start + 4);
  int ten = nyr_opt_const(f, 10);
  int twenty = nyr_opt_const(f, 20);
  nyr_opt_br(f, 10);

  nyr_opt_label(f, 10);
  size_t iv_phi = f->len;
  int iv = f->next_value;
  nyr_opt_phi(f, -1, initial, 14, initial);
  size_t sum_phi = f->len;
  int sum = f->next_value;
  nyr_opt_phi(f, -1, zero, 14, zero);
  int cond = nyr_opt_cmp(f, iv, limit, NYIR_CMP_LT);
  nyr_opt_br_if(f, cond, 11);
  nyr_opt_label(f, 20);
  nyr_opt_ret(f, sum);

  nyr_opt_label(f, 11);
  int parity = nyr_opt_binop(f, NYIR_AND_I64, iv, one);
  int is_even = nyr_opt_cmp(f, parity, zero, NYIR_CMP_EQ);
  nyr_opt_br_if(f, is_even, 13);
  nyr_opt_label(f, 12);
  int even_value = nyr_opt_binop(f, NYIR_ADD_I64, iv, ten);
  nyr_opt_br(f, 14);
  nyr_opt_label(f, 13);
  int odd_value = nyr_opt_binop(f, NYIR_ADD_I64, iv, twenty);
  nyr_opt_br(f, 14);
  nyr_opt_label(f, 14);
  int branch_value = f->next_value;
  nyr_opt_phi(f, 12, even_value, 13, odd_value);
  int contribution = nyr_opt_binop(f, NYIR_DIV_I64, branch_value, iv);
  int next_sum = nyr_opt_binop(f, NYIR_ADD_I64, sum, contribution);
  int next_iv = nyr_opt_binop(f, NYIR_ADD_I64, iv, one);
  nyr_opt_br(f, 10);

  f->data[iv_phi].phi_incoming[1].value = next_iv;
  f->data[sum_phi].phi_incoming[1].value = next_sum;
}

/*
 * Clone both an unlabeled in-loop fallthrough and an unlabeled loop exit.
 */
static void build_ssa_unlabeled_fallthrough(nyr_func_t *f) {
  nyr_opt_begin(f);
  int zero = nyr_opt_const(f, 0);
  int one = nyr_opt_const(f, 1);
  int trip = nyr_opt_const(f, 4);

  nyr_opt_label(f, 10);
  size_t iv_phi = f->len;
  int iv = f->next_value;
  nyr_opt_phi(f, -1, zero, 14, zero);
  size_t sum_phi = f->len;
  int sum = f->next_value;
  nyr_opt_phi(f, -1, zero, 14, zero);
  int keep_going = nyr_opt_cmp(f, iv, trip, NYIR_CMP_LT);
  nyr_opt_br_if(f, keep_going, 11);
  nyr_opt_ret(f, sum);

  nyr_opt_label(f, 11);
  int take_labeled_arm = nyr_opt_cmp(f, iv, one, NYIR_CMP_EQ);
  nyr_opt_br_if(f, take_labeled_arm, 13);
  nyr_opt_br(f, 14); /* false edge starts an unlabeled CFG block */
  nyr_opt_label(f, 13);
  nyr_opt_br(f, 14);

  nyr_opt_label(f, 14);
  int next_iv = nyr_opt_binop(f, NYIR_ADD_I64, iv, one);
  int next_sum = nyr_opt_binop(f, NYIR_ADD_I64, sum, next_iv);
  nyr_opt_br(f, 10);
  f->data[iv_phi].phi_incoming[1].value = next_iv;
  f->data[sum_phi].phi_incoming[1].value = next_sum;
}

static void build_ssa_multiblock_ordered_calls(nyr_func_t *f) {
  nyr_opt_begin(f);
  int zero = nyr_opt_const(f, 0);
  int one = nyr_opt_const(f, 1);
  int trip = nyr_opt_const(f, 4);
  int tag0 = nyr_opt_const(f, 101);
  int tag1 = nyr_opt_const(f, 102);
  int tag2 = nyr_opt_const(f, 103);
  int tag3 = nyr_opt_const(f, 104);
  int tag4 = nyr_opt_const(f, 105);
  nyr_opt_br(f, 10);

  nyr_opt_label(f, 10);
  size_t iv_phi = f->len;
  int iv = f->next_value;
  nyr_opt_phi(f, -1, zero, 14, zero);
  size_t sum_phi = f->len;
  int sum = f->next_value;
  nyr_opt_phi(f, -1, zero, 14, zero);
  int cond = nyr_opt_cmp(f, iv, trip, NYIR_CMP_LT);
  nyr_opt_br_if(f, cond, 11);
  nyr_opt_label(f, 20);
  nyr_opt_ret(f, sum);

  nyr_opt_label(f, 11);
  int parity = nyr_opt_binop(f, NYIR_AND_I64, iv, one);
  int is_even = nyr_opt_cmp(f, parity, zero, NYIR_CMP_EQ);
  nyr_opt_br_if(f, is_even, 13);
  nyr_opt_label(f, 12);
  nyr_opt_br(f, 14);
  nyr_opt_label(f, 13);
  nyr_opt_br(f, 14);
  nyr_opt_label(f, 14);
  int args[7] = {sum, iv, tag0, tag1, tag2, tag3, tag4};
  int *extra_args = malloc(sizeof(*extra_args));
  if (!extra_args)
    exit(2);
  extra_args[0] = args[6];
  int next_sum = nyr_ir_emit(
      f, (nyr_inst_t){
             .op = NYIR_CALL,
             .dst = -1,
             .a = args[0],
             .b = args[1],
             .c = args[2],
             .d = args[3],
             .e = args[4],
             .f = args[5],
             .imm = 7,
             .symbol = nyr_func_own_symbol_copy(f, "ordered"),
             .flags = NYIR_INST_F_EFFECTS_KNOWN,
             .effects = NYIR_EFFECT_CALL | NYIR_EFFECT_UNKNOWN_SIDE_EFFECT,
             .extra_args = extra_args,
             .extra_args_len = 1,
         });
  f->has_symbol_calls = true;
  int next_iv = nyr_opt_binop(f, NYIR_ADD_I64, iv, one);
  nyr_opt_br(f, 10);
  f->data[iv_phi].phi_incoming[1].value = next_iv;
  f->data[sum_phi].phi_incoming[1].value = next_sum;
}

typedef struct {
  size_t calls;
  bool valid;
} ordered_call_context_t;

static bool resolve_ordered_call(void *userdata, const char *symbol,
                                 const int64_t *args, size_t arg_count,
                                 int64_t *result, char *err, size_t err_len) {
  ordered_call_context_t *ctx = userdata;
  static const int64_t expected_sums[] = {0, 0, 1, 3};
  if (!ctx || !args || !result || !symbol || strcmp(symbol, "ordered") != 0 ||
      arg_count != 7 || ctx->calls >= 4) {
    if (err && err_len)
      snprintf(err, err_len, "unexpected ordered-call arguments");
    return false;
  }
  size_t i = ctx->calls;
  if (args[0] != expected_sums[i] || args[1] != (int64_t)i || args[2] != 101 ||
      args[3] != 102 || args[4] != 103 || args[5] != 104 || args[6] != 105)
    ctx->valid = false;
  *result = args[0] + args[1];
  ++ctx->calls;
  return true;
}

static bool eval_ordered_call_loop(nyr_func_t *f, int64_t *result) {
  nyr_eval_result_t evaluated = {0};
  ordered_call_context_t ctx = {.valid = true};
  char err[256] = {0};
  bool ok =
      nyr_ir_eval_with_calls(f, NULL, 0, 500, &evaluated, resolve_ordered_call,
                             &ctx, err, sizeof(err));
  if (!ok || !ctx.valid || ctx.calls != 4) {
    fprintf(stderr, "[loop-unroll-ssa] ordered call evaluation failed: %s\n",
            err[0] ? err : "call order or arguments changed");
    nyr_eval_result_free(&evaluated);
    return false;
  }
  *result = evaluated.result;
  nyr_eval_result_free(&evaluated);
  return true;
}

static bool check_ssa_unroll_budget(int64_t trip, bool expect_expanded) {
  nyr_func_t f;
  build_ssa_counted_sum(&f, trip, NYIR_CMP_LT);
  long long before = nyr_opt_eval(&f, "loop-unroll-ssa-budget-before");
  size_t original_len = f.len;
  bool ok = nyr_loop_unroll_ssa(&f) &&
            nyr_opt_verify(&f, "loop-unroll-ssa-budget-after");
  long long after = ok ? nyr_opt_eval(&f, "loop-unroll-ssa-budget-after") : -1;
  bool expanded = f.len > original_len;
  long long expected = 10 + trip * (trip - 1) / 2;
  nyr_ir_func_free(&f);
  if (!ok || before != expected || after != before ||
      expanded != expect_expanded) {
    fprintf(stderr,
            "[loop-unroll-ssa] trip budget=%lld before=%lld after=%lld "
            "expected=%lld expanded=%s expected-expanded=%s\n",
            (long long)trip, before, after, expected, expanded ? "yes" : "no",
            expect_expanded ? "yes" : "no");
    return false;
  }
  return true;
}

int main(void) {
  ny_dbg_init_from_verbosity(6);
  nyr_func_t f;
  build_loop_with_unrelated_phi(&f);
  if (!nyr_opt_verify(&f, "loop-unroll-before")) {
    nyr_ir_func_free(&f);
    return 1;
  }
  long long before = nyr_opt_eval(&f, "loop-unroll-before");
  size_t original_len = f.len;
  bool transformed = nyr_loop_unroll(&f);
  bool verified = nyr_opt_verify(&f, "loop-unroll-after");
  long long after = verified ? nyr_opt_eval(&f, "loop-unroll-after") : -1;
  bool expanded = f.len > original_len;
  nyr_ir_func_free(&f);

  if (!transformed || !verified || before != 4 || after != before ||
      !expanded) {
    fprintf(stderr,
            "[loop-unroll] unrelated PHI case before=%lld after=%lld "
            "expanded=%s\n",
            before, after, expanded ? "yes" : "no");
    return 1;
  }
  puts("[loop-unroll] post-loop PHI preserved; counted loop unrolled");

  build_loop_with_header_phi(&f);
  if (!nyr_opt_verify(&f, "loop-unroll-header-phi-before")) {
    nyr_ir_func_free(&f);
    return 1;
  }
  before = nyr_opt_eval(&f, "loop-unroll-header-phi-before");
  original_len = f.len;
  transformed = nyr_loop_unroll(&f);
  verified = nyr_opt_verify(&f, "loop-unroll-header-phi-after");
  after = verified ? nyr_opt_eval(&f, "loop-unroll-header-phi-after") : -1;
  expanded = f.len > original_len;
  if (!transformed || !verified || before != 4 || after != before || expanded) {
    fprintf(stderr,
            "[loop-unroll] header PHI case before=%lld after=%lld "
            "expanded=%s\n",
            before, after, expanded ? "yes" : "no");
    nyr_ir_func_free(&f);
    return 1;
  }
  if (!nyr_loop_unroll_ssa(&f) ||
      !nyr_opt_verify(&f, "loop-unroll-header-phi-ssa-after")) {
    nyr_ir_func_free(&f);
    return 1;
  }
  after = nyr_opt_eval(&f, "loop-unroll-header-phi-ssa-after");
  expanded = f.len > original_len;
  nyr_ir_func_free(&f);
  if (after != before || !expanded) {
    fprintf(stderr,
            "[loop-unroll-ssa] induction PHI before=%lld after=%lld "
            "expanded=%s\n",
            before, after, expanded ? "yes" : "no");
    return 1;
  }
  puts("[loop-unroll-ssa] induction PHI unrolled with matching result");

  build_ssa_counted_sum(&f, 4, NYIR_CMP_LT);
  before = nyr_opt_eval(&f, "loop-unroll-ssa-reduction-before");
  original_len = f.len;
  if (!nyr_loop_unroll_ssa(&f) ||
      !nyr_opt_verify(&f, "loop-unroll-ssa-reduction-after")) {
    nyr_ir_func_free(&f);
    return 1;
  }
  after = nyr_opt_eval(&f, "loop-unroll-ssa-reduction-after");
  expanded = f.len > original_len;
  nyr_ir_func_free(&f);
  if (before != 16 || after != before || !expanded) {
    fprintf(stderr,
            "[loop-unroll-ssa] reduction PHIs before=%lld after=%lld "
            "expanded=%s\n",
            before, after, expanded ? "yes" : "no");
    return 1;
  }
  puts("[loop-unroll-ssa] induction and reduction PHIs preserve result");

  build_ssa_counted_sum(&f, 3, NYIR_CMP_LE);
  before = nyr_opt_eval(&f, "loop-unroll-ssa-inclusive-before");
  original_len = f.len;
  if (!nyr_loop_unroll_ssa(&f) ||
      !nyr_opt_verify(&f, "loop-unroll-ssa-inclusive-after")) {
    nyr_ir_func_free(&f);
    return 1;
  }
  after = nyr_opt_eval(&f, "loop-unroll-ssa-inclusive-after");
  expanded = f.len > original_len;
  nyr_ir_func_free(&f);
  if (before != 16 || after != before || !expanded) {
    fprintf(stderr,
            "[loop-unroll-ssa] inclusive reduction before=%lld after=%lld "
            "expanded=%s\n",
            before, after, expanded ? "yes" : "no");
    return 1;
  }
  puts(
      "[loop-unroll-ssa] inclusive counted loop unrolled with matching result");

  build_ssa_strided_sum(&f);
  before = nyr_opt_eval(&f, "loop-unroll-ssa-strided-before");
  original_len = f.len;
  if (!nyr_loop_unroll_ssa(&f) ||
      !nyr_opt_verify(&f, "loop-unroll-ssa-strided-after")) {
    nyr_ir_func_free(&f);
    return 1;
  }
  after = nyr_opt_eval(&f, "loop-unroll-ssa-strided-after");
  expanded = f.len > original_len;
  nyr_ir_func_free(&f);
  if (before != 16 || after != before || !expanded) {
    fprintf(stderr,
            "[loop-unroll-ssa] positive-stride loop before=%lld after=%lld "
            "expanded=%s\n",
            before, after, expanded ? "yes" : "no");
    return 1;
  }
  puts("[loop-unroll-ssa] positive-stride induction preserves reduction");

  build_ssa_multiblock_branch_sum(&f, false);
  if (!nyr_opt_verify(&f, "loop-unroll-ssa-multiblock-before")) {
    nyr_ir_func_free(&f);
    return 1;
  }
  before = nyr_opt_eval(&f, "loop-unroll-ssa-multiblock-before");
  original_len = f.len;
  if (!nyr_loop_unroll_ssa(&f) ||
      !nyr_opt_verify(&f, "loop-unroll-ssa-multiblock-after")) {
    nyr_ir_func_free(&f);
    return 1;
  }
  after = nyr_opt_eval(&f, "loop-unroll-ssa-multiblock-after");
  expanded = f.len > original_len;
  nyr_ir_func_free(&f);
  if (before != 66 || after != before || !expanded) {
    fprintf(stderr,
            "[loop-unroll-ssa] multi-block branch PHI before=%lld after=%lld "
            "expanded=%s\n",
            before, after, expanded ? "yes" : "no");
    return 1;
  }
  puts("[loop-unroll-ssa] multi-block branch PHIs preserve result");

  build_ssa_multiblock_branch_sum(&f, true);
  if (!nyr_opt_verify(&f, "loop-unroll-ssa-constant-division-before")) {
    nyr_ir_func_free(&f);
    return 1;
  }
  before = nyr_opt_eval(&f, "loop-unroll-ssa-constant-division-before");
  original_len = f.len;
  if (!nyr_loop_unroll_ssa(&f) ||
      !nyr_opt_verify(&f, "loop-unroll-ssa-constant-division-after")) {
    nyr_ir_func_free(&f);
    return 1;
  }
  after = nyr_opt_eval(&f, "loop-unroll-ssa-constant-division-after");
  expanded = f.len > original_len;
  nyr_ir_func_free(&f);
  if (before != 32 || after != before || !expanded) {
    fprintf(stderr,
            "[loop-unroll-ssa] positive constant divisor before=%lld "
            "after=%lld expanded=%s\n",
            before, after, expanded ? "yes" : "no");
    return 1;
  }
  puts("[loop-unroll-ssa] positive constant divisor preserves result");

  build_ssa_induction_division(&f, 2);
  if (!nyr_opt_verify(&f,
                      "loop-unroll-ssa-positive-induction-divisor-before")) {
    nyr_ir_func_free(&f);
    return 1;
  }
  before =
      nyr_opt_eval(&f, "loop-unroll-ssa-positive-induction-divisor-before");
  original_len = f.len;
  if (!nyr_loop_unroll_ssa(&f) ||
      !nyr_opt_verify(&f, "loop-unroll-ssa-positive-induction-divisor-after")) {
    nyr_ir_func_free(&f);
    return 1;
  }
  after = nyr_opt_eval(&f, "loop-unroll-ssa-positive-induction-divisor-after");
  expanded = f.len > original_len;
  nyr_ir_func_free(&f);
  if (before != 24 || after != before || !expanded) {
    fprintf(stderr,
            "[loop-unroll-ssa] positive induction divisor before=%lld "
            "after=%lld expanded=%s\n",
            before, after, expanded ? "yes" : "no");
    return 1;
  }
  puts("[loop-unroll-ssa] proven positive induction divisor preserves result");

  build_ssa_induction_division(&f, 0);
  if (!nyr_opt_verify(&f, "loop-unroll-ssa-zero-induction-divisor-before")) {
    nyr_ir_func_free(&f);
    return 1;
  }
  original_len = f.len;
  if (!nyr_loop_unroll_ssa(&f) ||
      !nyr_opt_verify(&f, "loop-unroll-ssa-zero-induction-divisor-after")) {
    nyr_ir_func_free(&f);
    return 1;
  }
  expanded = f.len > original_len;
  nyr_ir_func_free(&f);
  if (expanded) {
    fputs("[loop-unroll-ssa] zero induction divisor was unrolled\n", stderr);
    return 1;
  }
  puts("[loop-unroll-ssa] zero induction divisor remains rolled");

  build_ssa_unlabeled_fallthrough(&f);
  if (!nyr_opt_verify(&f, "loop-unroll-ssa-unlabeled-before")) {
    nyr_ir_func_free(&f);
    return 1;
  }
  before = nyr_opt_eval(&f, "loop-unroll-ssa-unlabeled-before");
  original_len = f.len;
  if (!nyr_loop_unroll_ssa(&f) ||
      !nyr_opt_verify(&f, "loop-unroll-ssa-unlabeled-after")) {
    nyr_ir_func_free(&f);
    return 1;
  }
  after = nyr_opt_eval(&f, "loop-unroll-ssa-unlabeled-after");
  expanded = f.len > original_len;
  nyr_ir_func_free(&f);
  if (before != 10 || after != before || !expanded) {
    fprintf(stderr,
            "[loop-unroll-ssa] unlabeled fallthrough before=%lld after=%lld "
            "expanded=%s\n",
            before, after, expanded ? "yes" : "no");
    return 1;
  }
  puts("[loop-unroll-ssa] cloned unlabeled fallthrough preserves result");

  build_ssa_multiblock_ordered_calls(&f);
  if (!nyr_opt_verify(&f, "loop-unroll-ssa-call-before")) {
    nyr_ir_func_free(&f);
    return 1;
  }
  int64_t call_before = -1, call_after = -1;
  if (!eval_ordered_call_loop(&f, &call_before)) {
    nyr_ir_func_free(&f);
    return 1;
  }
  original_len = f.len;
  if (!nyr_loop_unroll_ssa(&f) ||
      !nyr_opt_verify(&f, "loop-unroll-ssa-call-after") ||
      !eval_ordered_call_loop(&f, &call_after)) {
    nyr_ir_func_free(&f);
    return 1;
  }
  expanded = f.len > original_len;
  nyr_ir_func_free(&f);
  if (!expanded || call_before != 6 || call_after != call_before) {
    fprintf(stderr,
            "[loop-unroll-ssa] ordered calls before=%lld after=%lld "
            "expanded=%s\n",
            (long long)call_before, (long long)call_after,
            expanded ? "yes" : "no");
    return 1;
  }
  puts(
      "[loop-unroll-ssa] multi-block call order and stack arguments preserved");

  if (!check_ssa_unroll_budget(0, false) ||
      !check_ssa_unroll_budget(16, true) || !check_ssa_unroll_budget(17, false))
    return 1;
  puts("[loop-unroll-ssa] zero-trip and 16-trip budget boundary preserved");
  return 0;
}
