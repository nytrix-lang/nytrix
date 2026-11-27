/*
 * DCE must keep a fall-through block's label when only a successor PHI names
 * it. With label ids far above the instruction count, DCE skips its label
 * table and asks nyr_label_referenced() instead; that fallback once counted
 * only branch targets, merged the block into its predecessor, and left the
 * PHI naming a label that was no longer a CFG predecessor (seen after the
 * 500-step `dce` select chain collapsed to a few instructions).
 */
#include "opt.h"

static void build(nyr_func_t *f, int64_t mid, int64_t join) {
  nyr_opt_begin(f);
  int x = nyr_opt_emit_imm(f, NYIR_LOAD_LOCAL, 0);
  int c = nyr_opt_cmp(f, x, nyr_opt_const(f, 0), NYIR_CMP_EQ);
  nyr_opt_br_if(f, c, join);
  nyr_opt_label(f, mid); /* reached only by fall-through */
  int y = nyr_opt_binop(f, NYIR_ADD_I64, x, nyr_opt_const(f, 1));
  nyr_opt_label(f, join);
  nyr_opt_phi(f, -1, x, mid, y);
  int p = f->next_value - 1;
  nyr_opt_ret(f, p);
}

static bool check(int64_t mid, int64_t join) {
  nyr_func_t f;
  build(&f, mid, join);
  bool ok = nyr_opt_verify(&f, "dce-phi-label-before") && nyr_dce(&f) &&
            nyr_opt_verify(&f, "dce-phi-label-after");
  static const int64_t inputs[] = {0, 5, -7};
  for (size_t k = 0; ok && k < sizeof(inputs) / sizeof(inputs[0]); ++k) {
    int64_t locals[1] = {inputs[k]}, got = 0;
    int64_t want = inputs[k] == 0 ? 0 : inputs[k] + 1;
    ok = nyr_opt_eval_locals(&f, "dce-phi-label", locals, 1, &got) &&
         got == want;
  }
  if (!ok) {
    fprintf(stderr, "[dce-phi-label] labels %lld/%lld broke the PHI edge\n",
            (long long)mid, (long long)join);
    nyr_dump(stderr, &f, "dce-phi-label");
  }
  nyr_ir_func_free(&f);
  return ok;
}

int main(void) {
  /*
   * Small ids use DCE's label table; large ids take the fallback query.
   */
  bool ok = check(1, 2) && check(1500, 2000);
  if (ok)
    puts("[dce-phi-label] PHI-only predecessor labels survive DCE");
  return ok ? 0 : 1;
}
