/*
 * Preserve vector PHIs reached through deep copy chains and loop-carried PHIs.
 */
#include "opt.h"

#include <nyr/nyr.h>
#include <stdio.h>

int main(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int zero = nyr_opt_const(&f, 0);
  int condition = nyr_opt_const(&f, 1);
  int value = nyr_opt_emit_abc(&f, NYIR_VEC8_SET1_I64, zero, -1, -1);
  for (int i = 0; i < 32; ++i)
    value = nyr_opt_emit_abc(&f, NYIR_COPY, value, -1, -1);
  nyr_opt_br(&f, 1);
  nyr_opt_label(&f, 1);
  nyr_opt_phi(&f, -1, value, 3, value);
  size_t phi_at = f.len - 1;
  int phi_dst = f.data[phi_at].dst;
  f.data[phi_at].phi_incoming[1].value = phi_dst;
  nyr_opt_br_if(&f, condition, 2);
  nyr_opt_label(&f, 3);
  nyr_opt_br(&f, 1);
  nyr_opt_label(&f, 2);
  nyr_opt_ret(&f, phi_dst);

  bool ok = nyr_opt_verify(&f, "phi-elim-deep-vec8-before") &&
            nyr_phi_elim(&f) && nyr_opt_verify(&f, "phi-elim-deep-vec8-after");
  bool saw_load = false;
  bool saw_store = false;
  for (size_t i = 0; ok && i < f.len; ++i) {
    const nyr_inst_t *in = &f.data[i];
    saw_load |= in->dst == phi_dst && in->op == NYIR_VEC8_LOAD_I64;
    saw_store |= in->op == NYIR_VEC8_STORE_I64;
  }
  if (ok && (!saw_load || !saw_store)) {
    fprintf(stderr, "deep vector PHI did not retain its 32-byte home\n");
    ok = false;
  }
  nyr_func_destroy(&f);
  return ok ? 0 : 1;
}
