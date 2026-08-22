/*
 * Loop rotation proof pass. Canonical NyIR while loops already have a rotated
 * latch; this pass removes a redundant unconditional branch immediately before
 * a header label only when fallthrough reaches the same header.
 */
#include "code/native/ir/opt/loop_analysis.h"

bool nyir_loop_rotate(nyir_func_t *f) {
  if (!f || f->len < 2)
    return true;
  for (size_t i = 1; i < f->len; ++i) {
    if (f->data[i].op == NYIR_LABEL && f->data[i - 1].op == NYIR_BR &&
        f->data[i - 1].imm == f->data[i].imm)
      nyir_inst_discard(&f->data[i - 1]);
  }
  return true;
}
