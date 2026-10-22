/*
 * Memory combination seam.
 *
 * SLP owns the legality proof and the actual widening.  This pass is only a
 * cheap gate, so do not require the scalar memory operations to be textually
 * adjacent: real address/value calculations commonly sit between them.  A
 * block with two scalar i64/f64-flagged loads or stores is enough to hand the
 * function to SLP; SLP still proves contiguity, lane ownership, trapping
 * order, and intervening effects before committing a candidate.
 */
#include "code/backend/ir/internal.h"
#include "code/backend/ir/opt/util.h"
bool nyr_slp_vectorize(nyr_func_t *f);

static bool memcombine_scalar_mem(nyr_op_t op) {
  return op == NYIR_LOAD_I64 || op == NYIR_STORE_I64;
}

static bool memcombine_candidate(const nyr_func_t *f) {
  if (!f || f->len < 2)
    return false;
  size_t block_mem = 0;
  for (size_t i = 0; i < f->len; ++i) {
    nyr_op_t op = f->data[i].op;
    if (op == NYIR_LABEL || op == NYIR_BR || op == NYIR_BR_IF ||
        op == NYIR_RET) {
      block_mem = 0;
      continue;
    }
    if (memcombine_scalar_mem(op) && ++block_mem >= 2)
      return true;
  }
  return false;
}

bool nyr_memcombine(nyr_func_t *f) {
  if (!memcombine_candidate(f))
    return true;
  return nyr_slp_vectorize(f);
}
