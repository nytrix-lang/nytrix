#include "code/native/ir/opt/util.h"
#include "code/native/ir/internal.h"
#include "base/compat.h"
#include "base/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Dead Store Elimination: remove STORE_LOCAL followed by another     */
/* STORE_LOCAL to the same slot without an intervening observation.   */
/*                                                                    */
/* Walk each basic block backwards.  This is deliberately linear in   */
/* the instruction stream: the previous implementation scanned ahead  */
/* from every store, which made store-heavy generated code quadratic. */
/* Calls and control-flow boundaries remain conservative barriers.    */
/* ------------------------------------------------------------------ */

bool nyir_dead_store_elim(nyir_func_t *f) {
  if (!f || f->next_value <= 0)
    return true;
  size_t count = nyir_max_local(f);
  if (!count)
    return true;

  unsigned *pending_epoch = (unsigned *)calloc(count, sizeof(*pending_epoch));
  bool *observed = (bool *)calloc(count, sizeof(*observed));
  if (!pending_epoch || !observed) {
    free(pending_epoch);
    free(observed);
    return false;
  }
  /* A local has no externally visible identity unless its address is taken.
   * Once all direct loads have disappeared, every remaining store to it is
   * dead even across calls and CFG edges. */
  for (size_t i = 0; i < f->len; ++i) {
    const nyir_inst_t *in = &f->data[i];
    if ((in->op == NYIR_LOAD_LOCAL || in->op == NYIR_ADDR_LOCAL) &&
        in->imm >= 0 && (size_t)in->imm < count)
      observed[in->imm] = true;
  }
  unsigned epoch = 1;
  for (size_t i = f->len; i-- > 0;) {
    nyir_inst_t *in = &f->data[i];
    if (in->op == NYIR_LABEL || in->op == NYIR_BR ||
        in->op == NYIR_BR_IF || in->op == NYIR_RET ||
        in->op == NYIR_CALL) {
      /* Do not propagate facts across a control-flow boundary or a call. */
      if (++epoch == 0) {
        memset(pending_epoch, 0, count * sizeof(*pending_epoch));
        epoch = 1;
      }
      continue;
    }
    if (in->imm < 0 || (size_t)in->imm >= count)
      continue;
    size_t slot = (size_t)in->imm;
    if (in->op == NYIR_LOAD_LOCAL || in->op == NYIR_ADDR_LOCAL) {
      pending_epoch[slot] = 0;
      continue;
    }
    if (in->op != NYIR_STORE_LOCAL)
      continue;
    if (!observed[slot]) {
      nyir_inst_discard(in);
      continue;
    }
    if (pending_epoch[slot] == epoch)
      nyir_inst_discard(in);
    pending_epoch[slot] = epoch;
  }
  free(pending_epoch);
  free(observed);
  return true;
}
