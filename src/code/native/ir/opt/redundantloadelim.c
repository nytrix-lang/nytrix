#include "code/native/ir/opt/util.h"
#include "code/native/ir/internal.h"
#include "base/compat.h"
#include "base/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Redundant Load Elimination: after a STORE_LOCAL to slot S, if the  */
/* next LOAD_LOCAL from S has no intervening side effects, replace    */
/* with a COPY from the stored value.                                 */
/*                                                                    */
/* Carry the candidate store forward to every alias-safe load instead */
/* of rescanning from every store. This gives a block-local mem2reg   */
/* win while keeping generated store-heavy code linear.               */
/* ------------------------------------------------------------------ */

bool nyir_redundant_load_elim(nyir_func_t *f) {
  if (!f || f->next_value <= 0)
    return true;
  size_t count = nyir_max_local(f);
  if (!count)
    return true;

  int *available_value = (int *)malloc(count * sizeof(*available_value));
  unsigned *available_epoch =
      (unsigned *)calloc(count, sizeof(*available_epoch));
  unsigned *escaped_epoch =
      (unsigned *)calloc(count, sizeof(*escaped_epoch));
  if (!available_value || !available_epoch || !escaped_epoch) {
    free(available_value);
    free(available_epoch);
    free(escaped_epoch);
    return false;
  }
  unsigned epoch = 1;
  for (size_t i = 0; i < f->len; ++i) {
    nyir_inst_t *in = &f->data[i];
    if (in->op == NYIR_LABEL || in->op == NYIR_BR ||
        in->op == NYIR_BR_IF || in->op == NYIR_RET ||
        in->op == NYIR_CALL) {
      if (++epoch == 0) {
        memset(available_epoch, 0, count * sizeof(*available_epoch));
        memset(escaped_epoch, 0, count * sizeof(*escaped_epoch));
        epoch = 1;
      }
      continue;
    }
    if (in->imm < 0 || (size_t)in->imm >= count)
      continue;
    size_t slot = (size_t)in->imm;
    if (in->op == NYIR_ADDR_LOCAL) {
      available_epoch[slot] = 0;
      escaped_epoch[slot] = epoch;
      continue;
    }
    if (in->op == NYIR_STORE_LOCAL) {
      if (escaped_epoch[slot] != epoch) {
        available_value[slot] = in->a;
        available_epoch[slot] = epoch;
      }
      continue;
    }
    if (in->op == NYIR_LOAD_LOCAL && in->dst >= 0 &&
        escaped_epoch[slot] != epoch) {
      if (available_epoch[slot] == epoch) {
        in->op = NYIR_COPY;
        in->a = available_value[slot];
        in->b = -1;
        in->imm = 0;
        in->symbol = NULL;
        in->effects = NYIR_EFFECT_NONE;
      } else {
        available_value[slot] = in->dst;
        available_epoch[slot] = epoch;
      }
    }
  }
  free(available_value);
  free(available_epoch);
  free(escaped_epoch);
  return true;
}
