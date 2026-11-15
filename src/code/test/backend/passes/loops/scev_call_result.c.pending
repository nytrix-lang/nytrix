/*
 * Call-result ranges in the symbolic trip expression.
 *
 * `rt_tbuf_len_raw` and the other length helpers return a count and report
 * zero on failure, so their result is non-negative but has no static upper
 * bound. SCEV uses that range where an operand range would otherwise be
 * missing, which is what lets a loop over a runtime length record the
 * symbolic count `limit - 0`.
 *
 * An unknown user call has no such summary and must leave the count unknown.
 */
#include "code/backend/passes/analysis/loop.h"
#include "opt.h"

static int failures;

static int call(nyr_func_t *f, const char *symbol, int a) {
  return nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_CALL,
                                     .dst = -1,
                                     .a = a,
                                     .b = -1,
                                     .c = -1,
                                     .d = -1,
                                     .e = -1,
                                     .f = -1,
                                     .symbol = symbol,
                                     .imm = 1});
}

/*
 * entry: base = rt_list_new(8); limit = len(base)
 *        if (0 < limit) goto P else ret 0
 * P:     br H
 * H:     iv = phi(P: 0, B: next); if (iv < limit) br B else br X
 * B:     next = iv + 1; br H
 * X:     ret iv
 */
static size_t build_loop(nyr_func_t *f, const char *length_symbol) {
  nyr_opt_begin(f);
  int capacity = nyr_opt_const(f, 8);
  int base = call(f, "rt_list_new", capacity);
  int limit = call(f, length_symbol, base);
  int zero = nyr_opt_const(f, 0);
  int guard = nyr_opt_cmp(f, zero, limit, NYIR_CMP_LT);
  nyr_opt_br_if(f, guard, 1);
  nyr_opt_br(f, 9);
  nyr_opt_label(f, 9);
  nyr_opt_ret(f, zero);
  nyr_opt_label(f, 1);
  nyr_opt_br(f, 2);
  nyr_opt_label(f, 2);
  int iv = f->next_value;
  nyr_opt_phi(f, 1, zero, 3, zero);
  size_t phi = f->len - 1;
  int cond = nyr_opt_cmp(f, iv, limit, NYIR_CMP_LT);
  nyr_opt_br_if(f, cond, 3);
  nyr_opt_br(f, 4);
  nyr_opt_label(f, 3);
  int next = nyr_opt_binop(f, NYIR_ADD_I64, iv, nyr_opt_const(f, 1));
  f->data[phi].phi_incoming[1].value = next;
  nyr_opt_br(f, 2);
  nyr_opt_label(f, 4);
  nyr_opt_ret(f, iv);
  return phi;
}

static const nyr_scev_loop_t *find_loop(const nyr_scev_info_t *info, int iv) {
  for (size_t i = 0; i < info->count; ++i)
    if (info->loops[i].iv == iv)
      return &info->loops[i];
  return NULL;
}

static bool check_case(const char *what, const char *symbol, bool want_known) {
  nyr_func_t f;
  size_t phi = build_loop(&f, symbol);
  int iv = f.data[phi].dst;
  nyr_scev_info_t info = {0};
  bool ok = nyr_opt_verify(&f, what) && nyr_scev_analyze(&f, &info);
  const nyr_scev_loop_t *loop = ok ? find_loop(&info, iv) : NULL;
  ok = ok && loop && loop->trip_expr_known == want_known &&
       !loop->trip_count_known;
  if (want_known && loop) {
    ok = ok && loop->trip_expr_const == 0 && loop->exit_value_known &&
         loop->exit_lb_known && loop->exit_lb == 1;
  } else if (loop) {
    ok = ok && !loop->exit_value_known;
  }
  if (!ok) {
    fprintf(stderr,
            "[scev-call] %s: known=%d trip=%d exit=%d pos=%d neg=%d\n", what,
            loop ? (int)loop->trip_expr_known : -1,
            loop ? (int)loop->trip_count_known : -1,
            loop ? (int)loop->exit_value_known : -1,
            loop ? loop->trip_expr_pos : -1, loop ? loop->trip_expr_neg : -1);
    nyr_dump(stderr, &f, what);
    ++failures;
  }
  nyr_scev_free(&info);
  nyr_ir_func_free(&f);
  return ok;
}

int main(void) {
  check_case("known-length", "rt_tbuf_len_raw", true);
  check_case("unknown-call", "user_defined_len", false);
  if (failures) {
    fprintf(stderr, "[scev-call] %d case(s) failed\n", failures);
    return 1;
  }
  printf("[scev-call] runtime length result has a non-negative range; unknown "
         "call does not\n");
  return 0;
}
