/*
 * Range based 32-bit narrowing boundaries. In particular, SHA-256 words use
 * the full uint32 range, and masking an arbitrary i64 by 0xffffffff is an
 * exact low-word operation even when the source itself has unknown range.
 */
#include "support.h"
#include <limits.h>
#include <stdint.h>

static const nyr_inst_t *find_def(const nyr_func_t *f, int value) {
  for (size_t i = 0; i < f->len; ++i)
    if (f->data[i].dst == value)
      return &f->data[i];
  return NULL;
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

int main(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);

  int signed_edge = nyr_opt_binop(
      &f, NYIR_ADD_I64, nyr_opt_const(&f, INT32_MAX), nyr_opt_const(&f, 1));
  int u32_edge =
      nyr_opt_binop(&f, NYIR_ADD_I64, nyr_opt_const(&f, (int64_t)INT32_MAX + 1),
                    nyr_opt_const(&f, 0));
  int u32_overflow =
      nyr_opt_binop(&f, NYIR_ADD_I64, nyr_opt_const(&f, (int64_t)UINT32_MAX),
                    nyr_opt_const(&f, 1));
  int unknown = load_local(&f, 0);
  int mask = nyr_opt_const(&f, (int64_t)UINT32_MAX);
  int masked = nyr_opt_binop(&f, NYIR_AND_I64, unknown, mask);
  int input = load_local(&f, 1);
  int first = nyr_opt_binop(&f, NYIR_ADD_I64, input, nyr_opt_const(&f, 7));
  int second = nyr_opt_binop(&f, NYIR_ADD_I64, first, nyr_opt_const(&f, 11));
  int masked_chain = nyr_opt_binop(&f, NYIR_AND_I64, second, mask);
  /*
   * This wide observer must keep `first` from being narrowed.
   */
  (void)nyr_opt_binop(&f, NYIR_MUL_I64, first, nyr_opt_const(&f, 3));
  nyr_opt_ret(&f, masked);

  if (!nyr_opt_verify(&f, "narrow/before") || !nyr_narrow(&f) ||
      !nyr_opt_verify(&f, "narrow/after"))
    return 1;
  const nyr_inst_t *a = find_def(&f, signed_edge);
  const nyr_inst_t *b = find_def(&f, u32_edge);
  const nyr_inst_t *c = find_def(&f, u32_overflow);
  const nyr_inst_t *d = find_def(&f, masked);
  const nyr_inst_t *e = find_def(&f, first);
  const nyr_inst_t *g = find_def(&f, second);
  const nyr_inst_t *h = find_def(&f, masked_chain);
  if (!a || !(a->flags & NYIR_INST_F_NARROW32) || !b ||
      !(b->flags & NYIR_INST_F_NARROW32) || !c ||
      (c->flags & NYIR_INST_F_NARROW32) || !d ||
      !(d->flags & NYIR_INST_F_NARROW32) || !e ||
      (e->flags & NYIR_INST_F_NARROW32) || !g ||
      !(g->flags & NYIR_INST_F_NARROW32) || !h ||
      !(h->flags & NYIR_INST_F_NARROW32)) {
    fprintf(stderr, "[narrow] uint32 proof boundary mismatch\n");
    nyr_ir_func_free(&f);
    return 1;
  }

  long long result = nyr_opt_eval(&f, "narrow/evaluate");
  nyr_ir_func_free(&f);
  if (result != 0) {
    fprintf(stderr, "[narrow] evaluation mismatch: %lld\n", result);
    return 1;
  }
  nyr_opt_check("narrow", 0, result, 0);
  return 0;
}
