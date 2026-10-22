/*
 * Driver for nyr_null_check_elim.
 *
 * One of this cycle's fixes removed a false-positive nonnull proof that
 * fired when an induction variable started at zero; this driver pins
 * the pass with a load/store loop whose IV does exactly that.  The
 * program must survive untouched: same verifier verdict, same result
 * (0+0+1+2+3 = 6 for n=4).
 */
#include "support.h"

/*
 * [entry][H guard fall->exit][B latch][exit ret] with local0 as the
 * running sum, starting at zero alongside an IV that starts at zero.
 */
static void build_local_sum(nyr_func_t *f, int64_t n) {
  nyr_opt_begin(f);
  int vn = nyr_opt_const(f, n);
  int zero = nyr_opt_const(f, 0);
  int one = nyr_opt_const(f, 1);
  nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_STORE_LOCAL,
                             .dst = -1,
                             .a = zero,
                             .b = -1,
                             .c = -1,
                             .d = -1,
                             .e = -1,
                             .f = -1,
                             .imm = 0});
  nyr_opt_br(f, 10);
  nyr_opt_label(f, 10);
  int iv = f->next_value;
  nyr_opt_phi(f, -1, zero, 0, zero); /* backedge patched below */
  int c = nyr_opt_cmp(f, iv, vn, NYIR_CMP_LT);
  nyr_opt_br_if(f, c, 11);
  /*
   * exit
   */
  nyr_opt_label(f, 30);
  int ld = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                      .dst = -1,
                                      .a = -1,
                                      .b = -1,
                                      .c = -1,
                                      .d = -1,
                                      .e = -1,
                                      .f = -1,
                                      .imm = 0});
  nyr_opt_ret(f, ld);
  /*
   * body: local0 += iv ; iv += 1
   */
  nyr_opt_label(f, 11);
  int prev = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                        .dst = -1,
                                        .a = -1,
                                        .b = -1,
                                        .c = -1,
                                        .d = -1,
                                        .e = -1,
                                        .f = -1,
                                        .imm = 0});
  int add = nyr_opt_binop(f, NYIR_ADD_I64, prev, iv);
  nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_STORE_LOCAL,
                             .dst = -1,
                             .a = add,
                             .b = -1,
                             .c = -1,
                             .d = -1,
                             .e = -1,
                             .f = -1,
                             .imm = 0});
  int ivn = nyr_opt_binop(f, NYIR_ADD_I64, iv, one);
  nyr_opt_br(f, 10);

  for (size_t i = 0; i < f->len; ++i) {
    if (f->data[i].op != NYIR_PHI)
      continue;
    f->data[i].phi_incoming[1] =
        (nyr_phi_incoming_t){.predecessor_label = 11, .value = ivn};
  }
}

static bool run_check_rewrite_preserves_debug(nyr_cmp_t cmp,
                                              nyr_op_t rewritten_op,
                                              int64_t expected) {
  static const char source[] = "null-check-origin.ny";
  nyr_func_t f;
  nyr_opt_begin(&f);
  int address = nyr_opt_const(&f, 1);
  int zero = nyr_opt_const(&f, 0);
  int cond = nyr_opt_cmp(&f, address, zero, cmp);
  size_t branch_at = f.len;
  nyr_opt_br_if(&f, cond, 10);
  f.data[branch_at].debug =
      (nyr_debug_loc_t){.file = source, .line = 7, .column = 13};
  nyr_opt_br(&f, 20);
  nyr_opt_label(&f, 10);
  int yes = nyr_opt_const(&f, 17);
  nyr_opt_ret(&f, yes);
  nyr_opt_label(&f, 20);
  int no = nyr_opt_const(&f, 29);
  nyr_opt_ret(&f, no);

  bool ok = nyr_opt_verify(&f, "nce/debug-before");
  int64_t before = ok ? nyr_opt_eval(&f, "nce/debug-before") : -1;
  ok =
      ok && nyr_null_check_elim(&f) && nyr_opt_verify(&f, "nce/debug-after") &&
      nyr_opt_eval(&f, "nce/debug-after") == expected && before == expected &&
      f.data[branch_at].op == rewritten_op &&
      f.data[branch_at].debug.file == source &&
      f.data[branch_at].debug.line == 7 && f.data[branch_at].debug.column == 13;
  if (!ok)
    fprintf(stderr,
            "[null-check-elim] check rewrite op=%d/%d value=%lld/%lld"
            " debug=%s:%u:%u\n",
            f.data[branch_at].op, rewritten_op,
            (long long)nyr_opt_eval(&f, "nce/debug-failure"),
            (long long)expected,
            f.data[branch_at].debug.file ? f.data[branch_at].debug.file : "-",
            f.data[branch_at].debug.line, f.data[branch_at].debug.column);
  if (!ok)
    nyr_dump(stderr, &f, "nce/debug-failure");
  nyr_ir_func_free(&f);
  return ok;
}

int main(void) {
  if (!run_check_rewrite_preserves_debug(NYIR_CMP_NE, NYIR_BR, 17) ||
      !run_check_rewrite_preserves_debug(NYIR_CMP_EQ, NYIR_NOP, 29))
    return 1;
  nyr_func_t f;
  build_local_sum(&f, 4);
  if (!nyr_opt_verify(&f, "nce/verify-before"))
    return 1;
  long long before = nyr_opt_eval(&f, "nce/before");
  nyr_null_check_elim(&f);
  if (!nyr_opt_verify(&f, "nce/verify-after"))
    return 1;
  long long after = nyr_opt_eval(&f, "nce/after");
  if (nyr_opt_want_dump())
    nyr_dump(stdout, &f, "after-nce");
  nyr_ir_func_free(&f);
  nyr_opt_check("null-check-elim", before, after, 6);
  return 0;
}
