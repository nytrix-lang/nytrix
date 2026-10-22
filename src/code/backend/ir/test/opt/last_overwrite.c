#include "code/backend/ir/opt/util.h"
#include "support.h"

extern bool nyr_loop_last_overwrite(nyr_func_t *f);

typedef enum {
  TEST_OVERWRITE,
  TEST_REDUCTION,
  TEST_TRAPPING,
  TEST_EARLY_EXIT,
} test_loop_kind_t;

static int load_local(nyr_func_t *f, int slot) {
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

static void build_loop(nyr_func_t *f, int64_t trip, test_loop_kind_t kind,
                       bool fallthrough_preheader, bool variable_trip,
                       int64_t initial_iv, bool inclusive_condition) {
  nyr_opt_begin(f);
  nyr_opt_label(f, 1);
  int limit = variable_trip ? load_local(f, 1) : nyr_opt_const(f, trip);
  int zero_iv = nyr_opt_const(f, initial_iv);
  int zero_state = nyr_opt_const(f, 0);
  int one = nyr_opt_const(f, 1);
  int two = nyr_opt_const(f, 2);
  if (!fallthrough_preheader)
    nyr_opt_br(f, 10);

  nyr_opt_label(f, 10);
  size_t iv_phi_index = f->len;
  int iv = f->next_value;
  nyr_opt_phi(f, 1, zero_iv, 14, zero_iv);
  size_t state_phi_index = f->len;
  int state = f->next_value;
  nyr_opt_phi(f, 1, zero_state, 14, zero_state);
  int cond = nyr_opt_cmp(f, iv, limit,
                         inclusive_condition ? NYIR_CMP_LE : NYIR_CMP_LT);
  nyr_opt_br_if(f, cond, 11);
  nyr_opt_br(f, 20);

  int next_state = -1;
  nyr_opt_label(f, 11);
  if (kind == TEST_EARLY_EXIT) {
    int stop = load_local(f, 2);
    nyr_opt_br_if(f, stop, 30);
  }
  if (kind == TEST_OVERWRITE || kind == TEST_EARLY_EXIT) {
    int flag = load_local(f, 0);
    nyr_opt_br_if(f, flag, 12);
    nyr_opt_br(f, 13);
    nyr_opt_label(f, 12);
    int plus = nyr_opt_binop(f, NYIR_ADD_I64, iv, one);
    nyr_opt_br(f, 14);
    nyr_opt_label(f, 13);
    int times = nyr_opt_binop(f, NYIR_MUL_I64, iv, two);
    nyr_opt_br(f, 14);
    nyr_opt_label(f, 14);
    next_state = f->next_value;
    nyr_opt_phi(f, 12, plus, 13, times);
  } else if (kind == TEST_REDUCTION) {
    next_state = nyr_opt_binop(f, NYIR_ADD_I64, state, iv);
    nyr_opt_br(f, 14);
    nyr_opt_label(f, 14);
  } else {
    next_state = nyr_opt_binop(f, NYIR_DIV_I64, one, iv);
    nyr_opt_br(f, 14);
    nyr_opt_label(f, 14);
  }
  int next_iv = nyr_opt_binop(f, NYIR_ADD_I64, iv, one);
  nyr_opt_br(f, 10);
  nyr_opt_label(f, 20);
  nyr_opt_ret(f, state);
  if (kind == TEST_EARLY_EXIT) {
    nyr_opt_label(f, 30);
    nyr_opt_ret(f, state);
  }
  f->data[iv_phi_index].phi_incoming[1].value = next_iv;
  f->data[state_phi_index].phi_incoming[1].value = next_state;
}

static bool evaluate(nyr_func_t *f, int64_t flag, bool *ok, int64_t *value,
                     size_t *comparisons) {
  int64_t locals[8] = {0};
  locals[0] = flag;
  locals[1] = 4;
  nyr_eval_result_t result = {0};
  char err[256] = {0};
  *ok = nyr_ir_eval(f, locals, 8, 500, &result, err, sizeof(err));
  if (*ok) {
    *value = result.result;
    if (comparisons)
      *comparisons = result.op_counts[NYIR_CMP_I64];
  }
  nyr_eval_result_free(&result);
  return true;
}

static bool check_overwrite(void) {
  for (int fallthrough = 0; fallthrough < 2; ++fallthrough) {
    nyr_func_t f;
    build_loop(&f, 4, TEST_OVERWRITE, fallthrough != 0, false, 0, false);
    if (!nyr_opt_verify(&f, "last-overwrite-before")) {
      nyr_ir_func_free(&f);
      return false;
    }
    int64_t before_true = 0, before_false = 0;
    bool eval_ok = false;
    evaluate(&f, 1, &eval_ok, &before_true, NULL);
    if (!eval_ok) {
      nyr_ir_func_free(&f);
      return false;
    }
    evaluate(&f, 0, &eval_ok, &before_false, NULL);
    if (!eval_ok || before_true != 4 || before_false != 6 ||
        !nyr_loop_last_overwrite(&f) ||
        !nyr_opt_verify(&f, "last-overwrite-after")) {
      nyr_ir_func_free(&f);
      return false;
    }
    int64_t after_true = 0, after_false = 0;
    size_t comparisons = 0;
    evaluate(&f, 1, &eval_ok, &after_true, &comparisons);
    if (!eval_ok || after_true != before_true || comparisons != 0) {
      fprintf(stderr,
              "[last-overwrite] fallthrough=%d flag=true result=%lld "
              "comparisons=%zu\n",
              fallthrough, (long long)after_true, comparisons);
      nyr_ir_func_free(&f);
      return false;
    }
    evaluate(&f, 0, &eval_ok, &after_false, NULL);
    bool pass = eval_ok && after_false == before_false;
    nyr_ir_func_free(&f);
    if (!pass) {
      fprintf(stderr, "[last-overwrite] fallthrough=%d flag=false mismatch\n",
              fallthrough);
      return false;
    }
  }
  printf("[last-overwrite] ok explicit/fallthrough preheaders outputs=4,6 "
         "comparisons=0\n");
  return true;
}

static bool check_unchanged(test_loop_kind_t kind, int64_t trip,
                            const char *tag, int64_t expected,
                            bool expect_trap) {
  nyr_func_t f;
  build_loop(&f, trip, kind, false, false, 0, false);
  if (!nyr_opt_verify(&f, tag)) {
    nyr_ir_func_free(&f);
    return false;
  }
  uint64_t before = nyr_debug_fingerprint(&f);
  int64_t result = 0;
  bool eval_ok = false;
  evaluate(&f, 0, &eval_ok, &result, NULL);
  bool before_matches = expect_trap ? !eval_ok : eval_ok && result == expected;
  bool pass_ok = nyr_loop_last_overwrite(&f) && nyr_opt_verify(&f, tag);
  uint64_t after = nyr_debug_fingerprint(&f);
  evaluate(&f, 0, &eval_ok, &result, NULL);
  bool after_matches = expect_trap ? !eval_ok : eval_ok && result == expected;
  bool pass = before_matches && pass_ok && after_matches && before == after;
  if (!pass)
    fprintf(stderr, "[%s] unsafe transform or behavior change\n", tag);
  else
    printf("[%s] unchanged and verified\n", tag);
  nyr_ir_func_free(&f);
  return pass;
}

static bool check_rejected_candidate(test_loop_kind_t kind, int64_t trip,
                                     bool variable_trip, int64_t initial_iv,
                                     bool inclusive_condition,
                                     const char *tag) {
  nyr_func_t f;
  build_loop(&f, trip, kind, false, variable_trip, initial_iv,
             inclusive_condition);
  if (!nyr_opt_verify(&f, tag)) {
    nyr_ir_func_free(&f);
    return false;
  }
  uint64_t before = nyr_debug_fingerprint(&f);
  if (!nyr_loop_last_overwrite(&f) || !nyr_opt_verify(&f, tag)) {
    nyr_ir_func_free(&f);
    return false;
  }
  uint64_t after = nyr_debug_fingerprint(&f);
  bool unchanged = before == after;
  if (!unchanged)
    fprintf(stderr, "[%s] unsupported loop was rewritten\n", tag);
  else
    printf("[%s] rejected without changing IR\n", tag);
  nyr_ir_func_free(&f);
  return unchanged;
}

int main(void) {
  return check_overwrite() &&
                 check_unchanged(TEST_OVERWRITE, 0, "last-overwrite-zero", 0,
                                 false) &&
                 check_unchanged(TEST_REDUCTION, 4, "last-overwrite-reduction",
                                 6, false) &&
                 check_unchanged(TEST_TRAPPING, 4, "last-overwrite-trapping", 0,
                                 true) &&
                 check_rejected_candidate(TEST_OVERWRITE, 4, true, 0, false,
                                          "last-overwrite-variable-trip") &&
                 check_rejected_candidate(TEST_OVERWRITE, INT64_MAX, false,
                                          INT64_MAX - 1, true,
                                          "last-overwrite-exit-overflow") &&
                 check_rejected_candidate(TEST_EARLY_EXIT, 4, false, 0, false,
                                          "last-overwrite-multiple-exits")
             ? 0
             : 1;
}
