/*
 * Redundant-load elimination: removes memory loads when the same
 * location was recently loaded and no intervening store can alias it.
 */
#include "code/native/ir/opt/util.h"
#include "code/native/ir/internal.h"
#include "base/compat.h"
#include "base/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Redundant Load Elimination: after a STORE_LOCAL to slot S, if the
 * next LOAD_LOCAL from S has no intervening side effects, replace
 * with a COPY from the stored value.
 *
 * Carry the candidate store forward to every alias-safe load instead
 * of rescanning from every store. This gives a block-local mem2reg
 * win while keeping generated store-heavy code linear.
 */

bool nyir_redundant_load_elim(nyir_func_t *f) {
  if (!f || f->next_value <= 0)
    return true;
  size_t count = nyir_max_local(f);
  if (!count)
    return true;

  int *available_value = (int *)malloc(count * sizeof(*available_value));
  unsigned *available_epoch =
      (unsigned *)calloc(count, sizeof(*available_epoch));
  bool *escaped = (bool *)calloc(count, sizeof(*escaped));
  if (!available_value || !available_epoch || !escaped) {
    free(available_value);
    free(available_epoch);
    free(escaped);
    return false;
  }
  /*
   * A local whose address is never materialized cannot be observed or
   * clobbered by a call.  Compute this once for the whole function instead
   * of treating every call as a blanket local-memory barrier.  Conversely,
   * never forward an address-taken local here: generic pointer loads/stores
   * may alias it even without a call.
   */
  for (size_t i = 0; i < f->len; ++i) {
    const nyir_inst_t *in = &f->data[i];
    if (in->op == NYIR_ADDR_LOCAL && in->imm >= 0 &&
        (size_t)in->imm < count)
      escaped[in->imm] = true;
  }
  #define NY_MEM_FORWARD_MAX 64
  typedef struct {
    int addr;
    int value;
    unsigned flags;
  } ny_mem_avail_t;
  ny_mem_avail_t mem_avail[NY_MEM_FORWARD_MAX];
  size_t mem_avail_count = 0;

  unsigned epoch = 1;
  for (size_t i = 0; i < f->len; ++i) {
    nyir_inst_t *in = &f->data[i];
    if (in->op == NYIR_LABEL || in->op == NYIR_BR ||
        in->op == NYIR_BR_IF || in->op == NYIR_RET) {
      mem_avail_count = 0;
      if (++epoch == 0) {
        memset(available_epoch, 0, count * sizeof(*available_epoch));
        epoch = 1;
      }
      continue;
    }
    if (in->op == NYIR_CALL) {
      mem_avail_count = 0;
      continue;
    }
    if (in->op == NYIR_STORE_I64) {
      int addr = in->a;
      int val = in->c;
      unsigned flags = in->flags & (NYIR_INST_F_MEM_F64 | NYIR_INST_F_MEM_BYTE);
      if (addr >= 0 && val >= 0) {
        bool found = false;
        for (size_t m = 0; m < mem_avail_count; ++m) {
          if (mem_avail[m].addr == addr) {
            mem_avail[m].value = val;
            mem_avail[m].flags = flags;
            found = true;
            break;
          }
        }
        if (!found && mem_avail_count < NY_MEM_FORWARD_MAX) {
          mem_avail[mem_avail_count++] = (ny_mem_avail_t){addr, val, flags};
        }
      }
      continue;
    }
    if (in->op == NYIR_LOAD_I64 && in->dst >= 0) {
      int addr = in->a;
      unsigned flags = in->flags & (NYIR_INST_F_MEM_F64 | NYIR_INST_F_MEM_BYTE);
      if (addr >= 0) {
        bool forwarded = false;
        for (size_t m = 0; m < mem_avail_count; ++m) {
          if (mem_avail[m].addr == addr && mem_avail[m].flags == flags) {
            in->op = NYIR_COPY;
            in->a = mem_avail[m].value;
            in->b = -1;
            in->c = -1;
            in->imm = 0;
            in->symbol = NULL;
            in->effects = NYIR_EFFECT_NONE;
            forwarded = true;
            break;
          }
        }
        if (!forwarded) {
          bool found = false;
          for (size_t m = 0; m < mem_avail_count; ++m) {
            if (mem_avail[m].addr == addr) {
              mem_avail[m].value = in->dst;
              mem_avail[m].flags = flags;
              found = true;
              break;
            }
          }
          if (!found && mem_avail_count < NY_MEM_FORWARD_MAX) {
            mem_avail[mem_avail_count++] = (ny_mem_avail_t){addr, in->dst, flags};
          }
        }
      }
      continue;
    }
    if (in->imm < 0 || (size_t)in->imm >= count)
      continue;
    size_t slot = (size_t)in->imm;
    if (in->op == NYIR_ADDR_LOCAL || escaped[slot]) {
      available_epoch[slot] = 0;
      continue;
    }
    if (in->op == NYIR_STORE_LOCAL) {
      available_value[slot] = in->a;
      available_epoch[slot] = epoch;
      continue;
    }
    if (in->op == NYIR_LOAD_LOCAL && in->dst >= 0) {
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
  #undef NY_MEM_FORWARD_MAX
  free(available_value);
  free(available_epoch);
  free(escaped);
  return true;
}
