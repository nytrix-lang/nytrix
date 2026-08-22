/*
 * Alias-store sinking: moves memory stores into the nearest
 * dominated block where the stored value is actually live.
 */
#include "code/native/ir/opt/util.h"
#include "code/native/ir/internal.h"
#include "base/compat.h"
#include "base/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Escape analysis enables scalar replacement of locals. Stores retain source
 * order because this pass has no complete alias proof for derived pointers.
 */


/*
 * Scalar replacement: promote non-escaping locals to registers.
 */
static bool scalar_replace(nyir_func_t *f, const nyir_local_escape_info_t *escapes,
                           size_t slot_count) {
  bool changed = false;
  /*
   * Any load of a local anywhere (including via loop back edges) means a
   * store is only dead when it is provably overwritten before any load.
   */
  bool *local_loaded = NULL;
  if (f && f->len) {
    local_loaded = calloc(slot_count, sizeof(bool));
    if (local_loaded) {
      for (size_t i = 0; i < f->len; ++i) {
        if (f->data[i].op == NYIR_LOAD_LOCAL && f->data[i].imm >= 0 &&
            (size_t)f->data[i].imm < f->len)
          local_loaded[f->data[i].imm] = true;
      }
    }
  }
  for (size_t i = 0; i < f->len; ++i) {
    nyir_inst_t *in = &f->data[i];
    if (in->op == NYIR_LOAD_LOCAL && in->imm >= 0 && (size_t)in->imm < slot_count) {
      if (!escapes[in->imm].escapes) {
        /*
         * Find the store that defines this value.  Stop at control-flow
         * barriers: a store in an earlier block does not dominate this
         * load (e.g. a diamond CFG where only one arm stores), and a
         * COPY from it would fail the dominance verifier.
         */
        int def = -1;
        for (size_t j = i; j > 0; --j) {
          nyir_op_t op = f->data[j - 1].op;
          if (op == NYIR_RET || nyir_is_barrier(op))
            break;
          if (op == NYIR_STORE_LOCAL &&
              f->data[j - 1].imm == (int)in->imm) {
            def = f->data[j - 1].a;
            break;
          }
        }
        if (def >= 0) {
          /*
           * Replace load with copy from defining value.
           */
          *in = (nyir_inst_t){.op = NYIR_COPY, .dst = in->dst, .a = def, .b = -1};
          changed = true;
        }
      }
    }
    if (in->op == NYIR_STORE_LOCAL && in->imm >= 0 && (size_t)in->imm < slot_count) {
      if (!escapes[in->imm].escapes) {
        /*
         * Dead store to a non-escaping local: safe only when the stored
         * value is never observed — either the local is never loaded
         * anywhere in the function, or a later store overwrites it before
         * any load.  The "never loaded" clause covers loop back edges,
         * where a load can precede the store in linear order yet still
         * read its value on the next iteration.
         */
        bool dead = false;
        /*
         * Allocation failure must be conservative: never assume the
         * local is unloaded.
         */
        if (local_loaded && !local_loaded[in->imm]) {
          dead = true;
        } else {
          /*
           * Overwrite-before-read is provable only within one block: a
           * later store in another block may be skipped by a branch.
           */
          for (size_t j = i + 1; j < f->len; ++j) {
            nyir_op_t op = f->data[j].op;
            if (op == NYIR_RET || nyir_is_barrier(op))
              break;
            if (op == NYIR_LOAD_LOCAL && f->data[j].imm == (int)in->imm)
              break; /* value observed later */
            if (op == NYIR_STORE_LOCAL && f->data[j].imm == (int)in->imm) {
              dead = true; /* overwritten before any load */
              break;
            }
          }
        }
        if (dead) {
          *in = (nyir_inst_t){.op = NYIR_NOP, .dst = -1, .a = -1, .b = -1};
          changed = true;
        }
      }
    }
  }
  free(local_loaded);
  return changed;
}

bool nyir_alias_store_sink(nyir_func_t *f) {
  if (!f || f->len < 2)
    return true;

  /*
   * Escape analysis + scalar replacement.
   */
  size_t slot_count = nyir_local_slot_count(f);
  nyir_local_escape_info_t *escapes =
      slot_count ? calloc(slot_count, sizeof(*escapes)) : NULL;
  if (slot_count && !escapes)
    return false;
  if (escapes) {
    if (!nyir_analyze_local_escapes(f, escapes, slot_count)) {
      free(escapes);
      return false;
    }
    scalar_replace(f, escapes, slot_count);
    free(escapes);
  }

  /*
   * Store movement requires an alias proof for derived pointers.
   */
  return true;
}