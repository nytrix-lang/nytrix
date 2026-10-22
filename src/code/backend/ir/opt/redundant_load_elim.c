/*
 * Redundant-load elimination: removes memory loads when the same
 * location was recently loaded and no intervening store can alias it.
 *
 * References:
 *  Cooper & Torczon — Engineering a Compiler, 2nd Ed., Ch.10 §10.3.
 *  Muchnick — Advanced Compiler Design and Implementation, Ch.13 §13.3.
 */
#include "base/common.h"
#include "base/compat.h"
#include "code/backend/ir/internal.h"
#include "code/backend/ir/opt/util.h"
/*
 * Redundant Load Elimination: after a STORE_LOCAL to slot S, if the
 * next LOAD_LOCAL from S has no intervening side effects, replace
 * with a COPY from the stored value.
 *
 * Carry the candidate store forward to every alias-safe load instead
 * of rescanning from every store. This gives a block-local mem2reg
 * win while keeping generated store-heavy code linear.
 */

bool nyr_redundant_load_elim(nyr_func_t *f) {
  if (!f || f->next_value <= 0)
    return true;
  size_t count = nyr_max_local(f);
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
    const nyr_inst_t *in = &f->data[i];
    if (in->dst < 0 || in->dst >= f->next_value)
      continue;
    switch (in->op) {
    case NYIR_ADDR_LOCAL:
    case NYIR_ADDR_SYMBOL:
    case NYIR_ALLOCA:
    case NYIR_ADD_I64:
    case NYIR_SUB_I64:
      value_alias[in->dst] = in->alias_class;
      break;
    case NYIR_COPY:
      /*
       * COPY carries value provenance. A load's alias_class describes the
       * address it reads, not a pointer value stored at that address.
       */
      if (in->a >= 0 && in->a < f->next_value)
        value_alias[in->dst] = value_alias[in->a];
      break;
    default:
      /*
       * Loaded pointers, call results, PHIs, and unknown producers remain
       * may-alias-anything until a dedicated value-provenance fact exists.
       */
      break;
    }
  }
  /*
   * A local whose address is never materialized cannot be observed or
   * clobbered by a call.  Compute this once for the whole function instead
   * of treating every call as a blanket local-memory barrier.  Conversely,
   * never forward an address-taken local here: generic pointer loads/stores
   * may alias it even without a call.
   */
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->op == NYIR_ADDR_LOCAL && in->imm >= 0 && (size_t)in->imm < count)
      escaped[in->imm] = true;
  }
#define NY_MEM_FORWARD_MAX 64
  typedef struct {
    int addr;
    int value;
    unsigned flags;
    nyr_scalar_class_t scalar_class;
  } ny_mem_avail_t;
  ny_mem_avail_t mem_avail[NY_MEM_FORWARD_MAX];
  size_t mem_avail_count = 0;

  unsigned epoch = 1;
  for (size_t i = 0; i < f->len; ++i) {
    nyr_inst_t *in = &f->data[i];
    if (in->op == NYIR_LABEL || in->op == NYIR_BR || in->op == NYIR_BR_IF ||
        in->op == NYIR_RET) {
      mem_avail_count = 0;
      if (++epoch == 0) {
        memset(available_epoch, 0, count * sizeof(*available_epoch));
        epoch = 1;
      }
      continue;
    }
    if (in->op == NYIR_CALL) {
      unsigned eff = nyr_effective_effects(in);
      if (eff & (NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_ALLOCATION |
                 NYIR_EFFECT_UNKNOWN_SIDE_EFFECT | NYIR_EFFECT_IO |
                 NYIR_EFFECT_FFI)) {
        mem_avail_count = 0;
      }
      /*
       * A call can mutate a local through an escaped address or captured
       * environment.  The escape summary is function-local and cannot see
       * aliases held by the callee, so do not carry STORE_LOCAL forwarding
       * facts across call boundaries.
       */
      memset(available_epoch, 0, count * sizeof(*available_epoch));
      continue;
    }
    if (in->op == NYIR_STORE_I64) {
      int addr = in->a;
      int val = in->c;
      unsigned flags = in->flags & NYIR_INST_F_MEM_BYTE;
      /*
       * A store through one SSA address kills every other cached
       * address entry: distinct SSA values can denote the same memory
       * and this block-local pass has no alias proof. Only the stored
       * address's own entry survives, updated to the new value.
       */
      size_t w = 0;
      uint32_t store_alias =
          addr >= 0 && addr < f->next_value ? value_alias[addr] : 0;
      bool stored_entry = false;
      for (size_t m = 0; m < mem_avail_count; ++m) {
        if (addr >= 0 && val >= 0 && mem_avail[m].addr == addr &&
            mem_avail[m].flags == flags &&
            mem_avail[m].scalar_class == in->scalar_class) {
          mem_avail[w].addr = addr;
          mem_avail[w].value = val;
          mem_avail[w].flags = flags;
          mem_avail[w].scalar_class = in->scalar_class;
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
            (ny_mem_avail_t){addr, val, flags, in->scalar_class};
      continue;
    }
    /*
     * Raw vector writes may hit any tracked address. Aggregate copy has two
     * independent memory operands: .a is its destination and .b is read-only
     * source. A known destination class invalidates only cached addresses in
     * that class; unknown destination or cached classes remain barriers.
     */
    if (in->op == NYIR_COPY_STRUCT) {
      uint32_t dest_alias =
          in->a >= 0 && in->a < f->next_value ? value_alias[in->a] : 0;
      if (!dest_alias) {
        mem_avail_count = 0;
      } else {
        size_t w = 0;
        for (size_t m = 0; m < mem_avail_count; ++m) {
          uint32_t cached_alias =
              mem_avail[m].addr >= 0 && mem_avail[m].addr < f->next_value
                  ? value_alias[mem_avail[m].addr]
                  : 0;
          if (!cached_alias || cached_alias == dest_alias)
            continue;
          mem_avail[w++] = mem_avail[m];
        }
        mem_avail_count = w;
      }
      continue;
    }
    if (in->op == NYIR_VEC4_STORE_F64 || in->op == NYIR_VEC8_STORE_F32 ||
        in->op == NYIR_VEC4_STORE_I64 || in->op == NYIR_VEC8_STORE_I64) {
      mem_avail_count = 0;
      continue;
    }
    if (in->op == NYIR_LOAD_I64 && in->dst >= 0 &&
        !(in->effects & NYIR_EFFECT_VOLATILE)) {
      int addr = in->a;
      unsigned flags = in->flags & NYIR_INST_F_MEM_BYTE;
      if (addr >= 0) {
        bool forwarded = false;
        for (size_t m = 0; m < mem_avail_count; ++m) {
          if (mem_avail[m].addr == addr && mem_avail[m].flags == flags &&
              mem_avail[m].scalar_class == in->scalar_class) {
            /*
             * Replace the load through the shared shape constructor so
             * metadata derived from the old memory read (notably range and
             * alias facts) cannot survive as facts about the copied value.
             */
            nir_make_copy(in, mem_avail[m].value);
            in->alias_class =
                mem_avail[m].value >= 0 && mem_avail[m].value < f->next_value
                    ? value_alias[mem_avail[m].value]
                    : 0;
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
            mem_avail[mem_avail_count++] =
                (ny_mem_avail_t){addr, in->dst, flags, in->scalar_class};
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
        int source = available_value[slot];
        uint32_t source_alias =
            source >= 0 && source < f->next_value ? value_alias[source] : 0;
        nir_make_copy(in, source);
        /*
         * The source fact was computed from value provenance above; the old
         * LOAD_LOCAL alias class described the slot access, not this value.
         */
        in->alias_class = source_alias;
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
