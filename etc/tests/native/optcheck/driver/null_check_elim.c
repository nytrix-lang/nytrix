/*
 * Driver for nyir_null_check_elim.
 *
 * One of this cycle's fixes removed a false-positive nonnull proof that
 * fired when an induction variable started at zero; this driver pins
 * the pass with a load/store loop whose IV does exactly that.  The
 * program must survive untouched: same verifier verdict, same result
 * (0+0+1+2+3 = 6 for n=4).
 */
#include "util.h"

/*
 * [entry][H guard fall->exit][B latch][exit ret] with local0 as the
 * running sum, starting at zero alongside an IV that starts at zero.
 */
static void build_local_sum(nyir_func_t *f, int64_t n) {
  ny_begin(f);
  int vn = ny_const(f, n);
  int zero = ny_const(f, 0);
  int one = ny_const(f, 1);
  nyir_emit(f, (nyir_inst_t){.op = NYIR_STORE_LOCAL, .dst = -1, .a = zero,
                             .b = -1, .c = -1, .d = -1, .e = -1, .f = -1,
                             .imm = 0});
  ny_br(f, 10);
  ny_label(f, 10);
  int iv = f->next_value;
  ny_phi(f, -1, zero, 0, zero); /* backedge patched below */
  int c = ny_cmp(f, iv, vn, NYIR_CMP_LT);
  ny_br_if(f, c, 11);
  /*
   * exit
   */
  ny_label(f, 30);
  int ld = nyir_emit(f, (nyir_inst_t){.op = NYIR_LOAD_LOCAL, .dst = -1,
                                      .a = -1, .b = -1, .c = -1, .d = -1,
                                      .e = -1, .f = -1, .imm = 0});
  ny_ret(f, ld);
  /*
   * body: local0 += iv ; iv += 1
   */
  ny_label(f, 11);
  int prev = nyir_emit(f, (nyir_inst_t){.op = NYIR_LOAD_LOCAL, .dst = -1,
                                        .a = -1, .b = -1, .c = -1, .d = -1,
                                        .e = -1, .f = -1, .imm = 0});
  int add = ny_binop(f, NYIR_ADD_I64, prev, iv);
  nyir_emit(f, (nyir_inst_t){.op = NYIR_STORE_LOCAL, .dst = -1, .a = add,
                             .b = -1, .c = -1, .d = -1, .e = -1, .f = -1,
                             .imm = 0});
  int ivn = ny_binop(f, NYIR_ADD_I64, iv, one);
  ny_br(f, 10);

  for (size_t i = 0; i < f->len; ++i) {
    if (f->data[i].op != NYIR_PHI)
      continue;
    f->data[i].phi_incoming[1] =
        (nyir_phi_incoming_t){.predecessor_label = 11, .value = ivn};
  }
}

int main(void) {
  nyir_func_t f;
  build_local_sum(&f, 4);
  if (!ny_verify(&f, "nce/verify-before"))
    return 1;
  long long before = ny_eval(&f, "nce/before");
  nyir_null_check_elim(&f);
  if (!ny_verify(&f, "nce/verify-after"))
    return 1;
  long long after = ny_eval(&f, "nce/after");
  if (ny_want_dump())
    nyir_dump(stdout, &f, "after-nce");
  nyir_func_free(&f);
  ny_check("null-check-elim", before, after, 6);
  return 0;
}
