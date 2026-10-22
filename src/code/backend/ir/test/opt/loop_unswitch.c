/*
 * Exercise both specialized outcomes of a canonical invariant loop branch.
 */
#include "base/debug.h"
#include "support.h"

extern bool nyr_loop_unswitch(nyr_func_t *f);

static bool build_invariant_branch(nyr_func_t *f, int64_t flag_value,
                                   bool condition_uses_iv, bool trap_false,
                                   bool return_iv, bool exit_phi,
                                   bool overflow_sum) {
  nyr_opt_begin(f);
  int base = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_ALLOCA,
                                        .dst = -1,
                                        .a = -1,
                                        .b = -1,
                                        .c = -1,
                                        .d = -1,
                                        .e = -1,
                                        .f = -1,
                                        .imm = 8,
                                        .scalar_class = NYIR_SCALAR_POINTER});
  int flag = nyr_opt_const(f, flag_value);
  int zero = nyr_opt_const(f, 0);
  int one = nyr_opt_const(f, 1);
  int sum_init = nyr_opt_const(f, overflow_sum ? INT64_MAX : 0);
  int limit = nyr_opt_const(f, 3);
  int one_value = nyr_opt_const(f, 11);
  int other_value = nyr_opt_const(f, 22);
  int exit_fallback = nyr_opt_const(f, -1);
  nyr_opt_br(f, 10);

  nyr_opt_label(f, 10);
  size_t phi_at = f->len;
  int iv = f->next_value;
  nyr_opt_phi(f, -1, zero, 40, zero);
  size_t sum_phi_at = f->len;
  int sum = f->next_value;
  nyr_opt_phi(f, -1, sum_init, 40, sum_init);
  int loop_cond = nyr_opt_cmp(f, iv, limit, NYIR_CMP_LT);
  nyr_opt_br_if(f, loop_cond, 11);
  if (!exit_phi)
    nyr_opt_br(f, 50);

  if (exit_phi) {
    nyr_opt_label(f, 50);
    int exit_value = f->next_value;
    nyr_opt_phi(f, 10, sum, 60, exit_fallback);
    nyr_opt_ret(f, exit_value);
  }

  nyr_opt_label(f, 11);
  int branch_cond =
      condition_uses_iv ? nyr_opt_cmp(f, iv, one, NYIR_CMP_LT) : flag;
  nyr_opt_br_if(f, branch_cond, 20);
  nyr_opt_br(f, 30);

  nyr_opt_label(f, 20);
  nyr_opt_br(f, 40);

  nyr_opt_label(f, 30);
  int false_value = other_value;
  if (trap_false)
    false_value =
        nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_CALL,
                                   .dst = -1,
                                   .a = -1,
                                   .b = -1,
                                   .c = -1,
                                   .d = -1,
                                   .e = -1,
                                   .f = -1,
                                   .imm = 0,
                                   .symbol = "unswitch_trap",
                                   .effects = NYIR_EFFECT_CALL |
                                              NYIR_EFFECT_UNKNOWN_SIDE_EFFECT |
                                              NYIR_EFFECT_MAY_TRAP});
  nyr_opt_br(f, 40);

  nyr_opt_label(f, 40);
  int selected_value = f->next_value;
  nyr_opt_phi(f, 20, one_value, 30, false_value);
  nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_STORE_I64,
                             .dst = -1,
                             .a = base,
                             .b = -1,
                             .c = selected_value,
                             .d = -1,
                             .e = -1,
                             .f = -1});
  int next_iv = nyr_opt_binop(f, NYIR_ADD_I64, iv, one);
  int next_sum = nyr_opt_binop(f, NYIR_ADD_I64, sum, one);
  nyr_opt_br(f, 10);

  if (!exit_phi) {
    nyr_opt_label(f, 50);
    int result = return_iv ? iv
                           : nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                                        .dst = -1,
                                                        .a = base,
                                                        .b = -1,
                                                        .c = -1,
                                                        .d = -1,
                                                        .e = -1,
                                                        .f = -1});
    nyr_opt_ret(f, result);
  }

  if (exit_phi) {
    nyr_opt_label(f, 60);
    nyr_opt_br(f, 50);
  }
  f->data[phi_at].phi_incoming[1].value = next_iv;
  f->data[sum_phi_at].phi_incoming[1].value = next_sum;
  return nyr_opt_verify(f, "loop-unswitch-before");
}

static bool check_unswitch(int64_t flag, bool dependent, long long want,
                           bool trap_false, bool return_iv, bool exit_phi,
                           bool overflow_sum) {
  nyr_func_t f;
  if (!build_invariant_branch(&f, flag, dependent, trap_false, return_iv,
                              exit_phi, overflow_sum))
    return false;
  if (nyr_opt_want_dump())
    nyr_dump_cfg(stderr, &f, "loop-unswitch-before");
  long long before = nyr_opt_eval(&f, "loop-unswitch-before");
  if (!nyr_loop_unswitch(&f) || !nyr_opt_verify(&f, "loop-unswitch-after")) {
    nyr_ir_func_free(&f);
    return false;
  }
  long long after = nyr_opt_eval(&f, "loop-unswitch-after");
  nyr_ir_func_free(&f);
  if (before != want || after != want) {
    fprintf(stderr,
            "[loop-unswitch] flag=%lld dependent=%d before=%lld after=%lld "
            "want=%lld\n",
            (long long)flag, (int)dependent, before, after, want);
    return false;
  }
  printf("[loop-unswitch] flag=%lld dependent=%d result=%lld\n",
         (long long)flag, (int)dependent, after);
  return true;
}

static bool check_multiple_exits(int64_t flag_value, long long expected) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int zero = nyr_opt_const(&f, 0);
  int one = nyr_opt_const(&f, 1);
  int limit = nyr_opt_const(&f, 2);
  int flag = nyr_opt_const(&f, flag_value);
  nyr_opt_br(&f, 10);
  nyr_opt_label(&f, 10);
  size_t phi_at = f.len;
  int iv = f.next_value;
  nyr_opt_phi(&f, -1, zero, 40, -1);
  int keep_going = nyr_opt_cmp(&f, iv, limit, NYIR_CMP_LT);
  nyr_opt_br_if(&f, keep_going, 11);
  nyr_opt_br(&f, 50);
  nyr_opt_label(&f, 11);
  nyr_opt_br_if(&f, flag, 20);
  nyr_opt_br(&f, 21);
  nyr_opt_label(&f, 20);
  int first_iteration = nyr_opt_cmp(&f, iv, one, NYIR_CMP_LT);
  nyr_opt_br_if(&f, first_iteration, 60);
  nyr_opt_br(&f, 40);
  nyr_opt_label(&f, 21);
  nyr_opt_br(&f, 40);
  nyr_opt_label(&f, 40);
  int next = nyr_opt_binop(&f, NYIR_ADD_I64, iv, one);
  nyr_opt_br(&f, 10);
  nyr_opt_label(&f, 50);
  nyr_opt_ret(&f, nyr_opt_const(&f, 17));
  nyr_opt_label(&f, 60);
  nyr_phi_incoming_t *exit_60_in = malloc(sizeof(*exit_60_in));
  if (!exit_60_in)
    exit(2);
  *exit_60_in = (nyr_phi_incoming_t){.predecessor_label = 20, .value = iv};
  int exit_60_iv = f.next_value;
  nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_PHI,
                              .dst = -1,
                              .phi_incoming = exit_60_in,
                              .phi_incoming_len = 1});
  nyr_opt_ret(
      &f, nyr_opt_binop(&f, NYIR_ADD_I64, exit_60_iv, nyr_opt_const(&f, 22)));
  f.data[phi_at].phi_incoming[1].value = next;

  bool ok = nyr_opt_verify(&f, "loop-unswitch-multiple-exits-before");
  long long before =
      ok ? nyr_opt_eval(&f, "loop-unswitch-multiple-exits-before") : 0;
  ok = ok && nyr_loop_unswitch(&f) &&
       nyr_opt_verify(&f, "loop-unswitch-multiple-exits-after");
  long long after =
      ok ? nyr_opt_eval(&f, "loop-unswitch-multiple-exits-after") : 0;
  ok = ok && before == expected && after == before;
  nyr_ir_func_free(&f);
  if (!ok)
    fprintf(stderr,
            "[loop-unswitch] multiple-exit PHI semantics changed "
            "(flag=%lld before=%lld after=%lld expected=%lld)\n",
            (long long)flag_value, before, after, expected);
  else
    printf("[loop-unswitch] multiple-exit semantics preserved "
           "(flag=%lld result=%lld)\n",
           (long long)flag_value, after);
  return ok;
}

static bool check_effectful_store(int64_t flag_value, long long expected) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int slot = nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_ALLOCA,
                                         .dst = -1,
                                         .a = -1,
                                         .b = -1,
                                         .c = -1,
                                         .d = -1,
                                         .e = -1,
                                         .f = -1,
                                         .imm = 8,
                                         .scalar_class = NYIR_SCALAR_POINTER});
  int zero = nyr_opt_const(&f, 0);
  int one = nyr_opt_const(&f, 1);
  int hundred = nyr_opt_const(&f, 100);
  int limit = nyr_opt_const(&f, 3);
  int flag = nyr_opt_const(&f, flag_value);
  nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_STORE_I64,
                              .dst = -1,
                              .a = slot,
                              .b = -1,
                              .c = zero,
                              .d = -1,
                              .e = -1,
                              .f = -1});
  nyr_opt_br(&f, 10);
  nyr_opt_label(&f, 10);
  size_t phi_at = f.len;
  int iv = f.next_value;
  nyr_opt_phi(&f, -1, zero, 40, -1);
  int keep_going = nyr_opt_cmp(&f, iv, limit, NYIR_CMP_LT);
  nyr_opt_br_if(&f, keep_going, 11);
  nyr_opt_br(&f, 50);
  nyr_opt_label(&f, 11);
  nyr_opt_br_if(&f, flag, 20);
  nyr_opt_br(&f, 30);
  nyr_opt_label(&f, 20);
  nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_STORE_I64,
                              .dst = -1,
                              .a = slot,
                              .b = -1,
                              .c = iv,
                              .d = -1,
                              .e = -1,
                              .f = -1});
  nyr_opt_br(&f, 40);
  nyr_opt_label(&f, 30);
  int false_value = nyr_opt_binop(&f, NYIR_ADD_I64, iv, hundred);
  nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_STORE_I64,
                              .dst = -1,
                              .a = slot,
                              .b = -1,
                              .c = false_value,
                              .d = -1,
                              .e = -1,
                              .f = -1});
  nyr_opt_label(&f, 40);
  int next = nyr_opt_binop(&f, NYIR_ADD_I64, iv, one);
  nyr_opt_br(&f, 10);
  nyr_opt_label(&f, 50);
  int result = nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                           .dst = -1,
                                           .a = slot,
                                           .b = -1,
                                           .c = -1,
                                           .d = -1,
                                           .e = -1,
                                           .f = -1});
  nyr_opt_ret(&f, result);
  f.data[phi_at].phi_incoming[1].value = next;

  bool ok = nyr_opt_verify(&f, "loop-unswitch-effects-before");
  long long before = ok ? nyr_opt_eval(&f, "loop-unswitch-effects-before") : 0;
  ok = ok && nyr_loop_unswitch(&f) &&
       nyr_opt_verify(&f, "loop-unswitch-effects-after");
  long long after = ok ? nyr_opt_eval(&f, "loop-unswitch-effects-after") : 0;
  ok = ok && before == expected && after == before;
  nyr_ir_func_free(&f);
  if (!ok)
    fprintf(stderr,
            "[loop-unswitch] branch-local store semantics changed "
            "(flag=%lld before=%lld after=%lld expected=%lld)\n",
            (long long)flag_value, before, after, expected);
  else
    printf("[loop-unswitch] branch-local store preserved (flag=%lld "
           "result=%lld)\n",
           (long long)flag_value, after);
  return ok;
}

static bool check_multiple_latches(int64_t flag_value, long long expected) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int slot = nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_ALLOCA,
                                         .dst = -1,
                                         .a = -1,
                                         .b = -1,
                                         .c = -1,
                                         .d = -1,
                                         .e = -1,
                                         .f = -1,
                                         .imm = 8,
                                         .scalar_class = NYIR_SCALAR_POINTER});
  int zero = nyr_opt_const(&f, 0);
  int one = nyr_opt_const(&f, 1);
  int hundred = nyr_opt_const(&f, 100);
  int limit = nyr_opt_const(&f, 3);
  int flag = nyr_opt_const(&f, flag_value);
  nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_STORE_I64,
                              .dst = -1,
                              .a = slot,
                              .b = -1,
                              .c = zero,
                              .d = -1,
                              .e = -1,
                              .f = -1});
  nyr_opt_br(&f, 10);
  nyr_opt_label(&f, 10);
  size_t phi_at = f.len;
  int iv = f.next_value;
  nyr_opt_phi(&f, -1, zero, 40, -1);
  nyr_phi_incoming_t *three_incoming = malloc(3 * sizeof(*three_incoming));
  if (!three_incoming) {
    nyr_ir_func_free(&f);
    return false;
  }
  three_incoming[0] = f.data[phi_at].phi_incoming[0];
  free(f.data[phi_at].phi_incoming);
  f.data[phi_at].phi_incoming = three_incoming;
  f.data[phi_at].phi_incoming[1] =
      (nyr_phi_incoming_t){.predecessor_label = 40, .value = -1};
  f.data[phi_at].phi_incoming[2] =
      (nyr_phi_incoming_t){.predecessor_label = 41, .value = -1};
  f.data[phi_at].phi_incoming_len = 3;
  int keep_going = nyr_opt_cmp(&f, iv, limit, NYIR_CMP_LT);
  nyr_opt_br_if(&f, keep_going, 11);
  nyr_opt_br(&f, 50);
  nyr_opt_label(&f, 11);
  nyr_opt_br_if(&f, flag, 20);
  nyr_opt_br(&f, 21);
  nyr_opt_label(&f, 20);
  nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_STORE_I64,
                              .dst = -1,
                              .a = slot,
                              .b = -1,
                              .c = iv,
                              .d = -1,
                              .e = -1,
                              .f = -1});
  nyr_opt_br(&f, 40);
  nyr_opt_label(&f, 21);
  int other = nyr_opt_binop(&f, NYIR_ADD_I64, iv, hundred);
  nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_STORE_I64,
                              .dst = -1,
                              .a = slot,
                              .b = -1,
                              .c = other,
                              .d = -1,
                              .e = -1,
                              .f = -1});
  nyr_opt_br(&f, 41);
  nyr_opt_label(&f, 40);
  int next_a = nyr_opt_binop(&f, NYIR_ADD_I64, iv, one);
  nyr_opt_br(&f, 10);
  nyr_opt_label(&f, 41);
  int next_b = nyr_opt_binop(&f, NYIR_ADD_I64, iv, one);
  nyr_opt_br(&f, 10);
  nyr_opt_label(&f, 50);
  int result = nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                           .dst = -1,
                                           .a = slot,
                                           .b = -1,
                                           .c = -1,
                                           .d = -1,
                                           .e = -1,
                                           .f = -1});
  nyr_opt_ret(&f, result);
  f.data[phi_at].phi_incoming[1].value = next_a;
  f.data[phi_at].phi_incoming[2].value = next_b;

  bool ok = nyr_opt_verify(&f, "loop-unswitch-multiple-latches-before");
  long long before =
      ok ? nyr_opt_eval(&f, "loop-unswitch-multiple-latches-before") : 0;
  ok = ok && nyr_loop_unswitch(&f) &&
       nyr_opt_verify(&f, "loop-unswitch-multiple-latches-after");
  long long after =
      ok ? nyr_opt_eval(&f, "loop-unswitch-multiple-latches-after") : 0;
  ok = ok && before == expected && after == before;
  nyr_ir_func_free(&f);
  if (!ok)
    fprintf(stderr,
            "[loop-unswitch] multiple-latch semantics changed "
            "(flag=%lld before=%lld after=%lld expected=%lld)\n",
            (long long)flag_value, before, after, expected);
  else
    printf("[loop-unswitch] multiple latches preserved (flag=%lld "
           "result=%lld)\n",
           (long long)flag_value, after);
  return ok;
}

typedef struct {
  size_t calls;
} unswitch_call_context_t;

static bool resolve_unswitch_trap(void *opaque, const char *symbol,
                                  const int64_t *args, size_t arg_count,
                                  int64_t *result, char *err, size_t err_len) {
  unswitch_call_context_t *ctx = opaque;
  (void)args;
  (void)result;
  if (!ctx || !symbol || strcmp(symbol, "unswitch_trap") != 0 || arg_count != 0)
    return false;
  ++ctx->calls;
  snprintf(err, err_len, "selected unswitch trap");
  return false;
}

static bool check_selected_call_trap(void) {
  nyr_func_t f;
  if (!build_invariant_branch(&f, 0, false, true, false, false, false))
    return false;
  int64_t locals[8] = {0};
  nyr_eval_result_t before = {0}, after = {0};
  char before_err[256] = {0}, after_err[256] = {0};
  unswitch_call_context_t before_ctx = {0}, after_ctx = {0};
  bool before_ok =
      nyr_ir_eval_with_calls(&f, locals, 8, 500, &before, resolve_unswitch_trap,
                           &before_ctx, before_err, sizeof(before_err));
  bool transformed =
      nyr_loop_unswitch(&f) &&
      nyr_opt_verify(&f, "loop-unswitch-selected-call-trap-after");
  bool after_ok =
      transformed &&
      nyr_ir_eval_with_calls(&f, locals, 8, 500, &after, resolve_unswitch_trap,
                           &after_ctx, after_err, sizeof(after_err));
  bool ok = !before_ok && !after_ok && transformed && before_ctx.calls == 1 &&
            after_ctx.calls == before_ctx.calls &&
            strcmp(before_err, after_err) == 0;
  nyr_eval_result_free(&before);
  nyr_eval_result_free(&after);
  nyr_ir_func_free(&f);
  if (!ok)
    fprintf(stderr,
            "[loop-unswitch] selected unknown-call trap changed "
            "(before-ok=%d after-ok=%d calls=%zu/%zu before=%s after=%s)\n",
            (int)before_ok, (int)after_ok, before_ctx.calls, after_ctx.calls,
            before_err, after_err);
  else
    printf("[loop-unswitch] selected unknown-call trap preserved\n");
  return ok;
}

static bool check_unlabelled_exit_semantics(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int zero = nyr_opt_const(&f, 0);
  int one = nyr_opt_const(&f, 1);
  int limit = nyr_opt_const(&f, 3);
  int flag = nyr_opt_const(&f, 1);
  int result = nyr_opt_const(&f, 17);
  nyr_opt_br(&f, 10);
  nyr_opt_label(&f, 10);
  size_t phi_at = f.len;
  int iv = f.next_value;
  nyr_opt_phi(&f, -1, zero, 40, -1);
  int keep_going = nyr_opt_cmp(&f, iv, limit, NYIR_CMP_LT);
  nyr_opt_br_if(&f, keep_going, 11);
  nyr_opt_ret(&f, result); /* Unlabelled fallthrough exit. */
  nyr_opt_label(&f, 11);
  nyr_opt_br_if(&f, flag, 20);
  nyr_opt_br(&f, 21);
  nyr_opt_label(&f, 20);
  nyr_opt_br(&f, 40);
  nyr_opt_label(&f, 21);
  nyr_opt_br(&f, 40);
  nyr_opt_label(&f, 40);
  int next = nyr_opt_binop(&f, NYIR_ADD_I64, iv, one);
  nyr_opt_br(&f, 10);
  f.data[phi_at].phi_incoming[1].value = next;

  bool ok = nyr_opt_verify(&f, "loop-unswitch-unlabelled-exit-before");
  long long before =
      ok ? nyr_opt_eval(&f, "loop-unswitch-unlabelled-exit-before") : 0;
  ok = ok && nyr_loop_unswitch(&f) &&
       nyr_opt_verify(&f, "loop-unswitch-unlabelled-exit-after");
  long long after =
      ok ? nyr_opt_eval(&f, "loop-unswitch-unlabelled-exit-after") : 0;
  ok = ok && before == 17 && after == before;
  nyr_ir_func_free(&f);
  if (!ok)
    fprintf(stderr,
            "[loop-unswitch] unlabelled fallthrough exit changed semantics "
            "(before=%lld after=%lld)\n",
            before, after);
  else
    printf("[loop-unswitch] unlabelled fallthrough exit semantics preserved\n");
  return ok;
}

static bool check_unlabelled_trampoline_semantics(int64_t flag_value,
                                                  bool true_exits,
                                                  int64_t trip_count,
                                                  long long expected) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int zero = nyr_opt_const(&f, 0);
  int one = nyr_opt_const(&f, 1);
  int limit = nyr_opt_const(&f, trip_count);
  int flag = nyr_opt_const(&f, flag_value);
  int normal_exit = nyr_opt_const(&f, 17);
  int early_exit = nyr_opt_const(&f, 23);
  nyr_opt_br(&f, 10);
  nyr_opt_label(&f, 10);
  size_t phi_at = f.len;
  int iv = f.next_value;
  nyr_opt_phi(&f, -1, zero, 40, -1);
  int keep_going = nyr_opt_cmp(&f, iv, limit, NYIR_CMP_LT);
  nyr_opt_br_if(&f, keep_going, 11);
  nyr_opt_br(&f, 50);
  nyr_opt_label(&f, 11);
  nyr_opt_br_if(&f, flag, true_exits ? 60 : 20);
  nyr_opt_br(&f, true_exits ? 20 : 60);
  nyr_opt_label(&f, 20);
  nyr_opt_br(&f, 40);
  nyr_opt_label(&f, 40);
  int next = nyr_opt_binop(&f, NYIR_ADD_I64, iv, one);
  nyr_opt_br(&f, 10);
  nyr_opt_label(&f, 50);
  nyr_opt_ret(&f, normal_exit);
  nyr_opt_label(&f, 60);
  nyr_opt_ret(&f, early_exit);
  f.data[phi_at].phi_incoming[1].value = next;

  bool ok = nyr_opt_verify(&f, "loop-unswitch-trampoline-before");
  long long before =
      ok ? nyr_opt_eval(&f, "loop-unswitch-trampoline-before") : 0;
  ok = ok && nyr_loop_unswitch(&f) &&
       nyr_opt_verify(&f, "loop-unswitch-trampoline-after");
  long long after = ok ? nyr_opt_eval(&f, "loop-unswitch-trampoline-after") : 0;
  ok = ok && before == expected && after == before;
  nyr_ir_func_free(&f);
  if (!ok)
    fprintf(stderr,
            "[loop-unswitch] unlabelled branch trampoline changed semantics "
            "(flag=%lld before=%lld after=%lld "
            "expected=%lld)\n",
            (long long)flag_value, before, after, expected);
  else
    printf("[loop-unswitch] unlabelled branch trampoline semantics preserved "
           "(flag=%lld result=%lld)\n",
           (long long)flag_value, after);
  return ok;
}

int main(void) {
  if (getenv("OPTCHECK_UNSWITCH_TRACE"))
    ny_dbg_set_flag(NY_DBG_HEURISTIC, true);
  if (!check_unswitch(1, false, 11, false, false, false, false) ||
      !check_unswitch(0, false, 22, false, false, false, false) ||
      !check_unswitch(0, true, 22, false, false, false, false) ||
      !check_unswitch(1, false, 11, true, false, false, false) ||
      !check_unswitch(1, false, 3, false, true, false, false) ||
      !check_unswitch(1, false, 3, false, false, true, false) ||
      !check_unswitch(1, false, INT64_MIN + 2, false, false, true, true) ||
      !check_multiple_exits(1, 22) || !check_multiple_exits(0, 17) ||
      !check_effectful_store(1, 2) || !check_effectful_store(0, 102) ||
      !check_multiple_latches(1, 2) || !check_multiple_latches(0, 102) ||
      !check_selected_call_trap() || !check_unlabelled_exit_semantics())
    return 1;
  return check_unlabelled_trampoline_semantics(1, false, 2, 17) &&
                 check_unlabelled_trampoline_semantics(0, false, 2, 23) &&
                 check_unlabelled_trampoline_semantics(1, true, 2, 23) &&
                 check_unlabelled_trampoline_semantics(0, true, 2, 17) &&
                 check_unlabelled_trampoline_semantics(0, false, 0, 17)
             ? 0
             : 1;
}
