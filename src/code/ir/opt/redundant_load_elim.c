/*
 * Redundant-load elimination: removes memory loads when the same
 * location was recently loaded and no intervening store can alias it.
 *
 * References:
 *  Cooper & Torczon — Engineering a Compiler, 2nd Ed., Ch.10 §10.3.
 *  Muchnick — Advanced Compiler Design and Implementation, Ch.13 §13.3.
 */
#include "code/ir/opt/util.h"
#include "code/ir/internal.h"
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
  uint32_t *value_alias =
      (uint32_t *)calloc((size_t)f->next_value, sizeof(*value_alias));
  if (!available_value || !available_epoch || !escaped || !value_alias) {
    free(available_value);
    free(available_epoch);
    free(escaped);
    free(value_alias);
    return false;
  }
  for (size_t i = 0; i < f->len; ++i) {
    const nyir_inst_t *in = &f->data[i];
    if (in->dst >= 0 && in->dst < f->next_value)
      value_alias[in->dst] = in->alias_class;
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
      unsigned eff = nyir_effective_effects(in);
      if (eff & (NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_ALLOCATION |
                 NYIR_EFFECT_UNKNOWN_SIDE_EFFECT | NYIR_EFFECT_IO |
                 NYIR_EFFECT_FFI)) {
        mem_avail_count = 0;
      }
      continue;
    }
    if (in->op == NYIR_STORE_I64) {
      int addr = in->a;
      int val = in->c;
      unsigned flags = in->flags & (NYIR_INST_F_MEM_F64 | NYIR_INST_F_MEM_BYTE);
      /*
       * A store through one SSA address kills every other cached
       * address entry: distinct SSA values can denote the same memory
       * and this block-local pass has no alias proof. Only the stored
       * address's own entry survives, updated to the new value.
       */
      size_t w = 0;
      uint32_t store_alias = addr >= 0 && addr < f->next_value
                                 ? value_alias[addr]
                                 : 0;
      bool stored_entry = false;
      for (size_t m = 0; m < mem_avail_count; ++m) {
        if (addr >= 0 && val >= 0 && mem_avail[m].addr == addr &&
            mem_avail[m].flags == flags) {
          mem_avail[w].addr = addr;
          mem_avail[w].value = val;
          mem_avail[w].flags = flags;
          ++w;
          stored_entry = true;
          continue;
        }
        uint32_t cached_alias =
            mem_avail[m].addr >= 0 && mem_avail[m].addr < f->next_value
                ? value_alias[mem_avail[m].addr]
                : 0;
        if (store_alias && cached_alias && store_alias != cached_alias)
          mem_avail[w++] = mem_avail[m];
      }
      mem_avail_count = w;
      if (!stored_entry && addr >= 0 && val >= 0 &&
          mem_avail_count < NY_MEM_FORWARD_MAX)
        mem_avail[mem_avail_count++] =
            (ny_mem_avail_t){addr, val, flags};
      continue;
    }
    /*
     * Raw wide writes (SIMD stores, struct copies) may hit any tracked
     * address; no alias facts here, so they flush the whole table.
     */
    if (in->op == NYIR_VEC4_STORE_F64 || in->op == NYIR_VEC8_STORE_F32 ||
        in->op == NYIR_VEC4_STORE_I64 || in->op == NYIR_VEC8_STORE_I64 ||
        in->op == NYIR_COPY_STRUCT) {
      mem_avail_count = 0;
      continue;
    }
    if (in->op == NYIR_LOAD_I64 && in->dst >= 0 &&
        !(in->effects & NYIR_EFFECT_VOLATILE)) {
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
            in->flags = 0;
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
        in->flags = 0;
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
  free(value_alias);
  return true;
}
