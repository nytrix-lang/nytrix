/*
 * Unified Multi-Tier SROA Engine:
 * Combines points-to promotion, escape analysis, dead store elimination,
 * and scalar/aggregate replacement into a single cohesive pipeline with
 * shared local escape analysis and minimal graph rescanning.
 */
#include "code/backend/ir/opt/util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool nyr_sroa_unified(nyr_func_t *f);
bool nyr_sroa_scalar(nyr_func_t *f);
bool nyr_escape_sroa(nyr_func_t *f);

/*
 * True when value v is defined by CONST_I64 0 (nil immediate).  Nil cannot
 * address a frame slot, so a PHI edge carrying it does not alias the slot
 * carried by the other edges.  Any other unmapped value (including other
 * constants) is treated as unknown to stay fail-closed.
 */
static bool sroa_value_is_null_const(const nyr_func_t *f, int v) {
  if (!f || v < 0 || v >= f->next_value)
    return false;
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->dst == v) {
      return in->op == NYIR_CONST_I64 && in->imm == 0;
    }
  }
  return false;
}

bool nyr_sroa_unified(nyr_func_t *f) {
  if (!f || f->len == 0)
    return true;

  size_t slot_count = nyr_local_slot_count(f);
  if (!slot_count)
    return true;

  /*
   * Tier 1: Local Points-To Promotion.
   * ADDR_LOCAL v -> local L. When LOAD_I64/STORE_I64 of that pointer
   * and L is never passed to CALL, rewrite to LOAD_LOCAL/STORE_LOCAL.
   */
  if (f->next_value > 0) {
    size_t nv = (size_t)f->next_value;
    int *pt = calloc(nv, sizeof(int));
    /*
     * Escape provenance (mark_pt) is tracked separately from rewrite
     * provenance (pt): a derived pointer exposes its base storage at any
     * offset, so marking must follow arithmetic and merges aggressively,
     * while rewriting must only fire on exact (zero-offset) derivations.
     * Over-marking costs optimization; under-marking miscompiles.
     */
    int *mark_pt = calloc(nv, sizeof(int));
    bool *pt_escaped = calloc(slot_count, sizeof(bool));
    if (!pt || !mark_pt || !pt_escaped) {
      free(pt);
      free(mark_pt);
      free(pt_escaped);
    } else {
      for (size_t i = 0; i < nv; ++i) {
        pt[i] = -1;
        mark_pt[i] = -1;
      }
      for (size_t i = 0; i < f->len; ++i) {
        const nyr_inst_t *in = &f->data[i];
        if (in->op == NYIR_ADDR_LOCAL && in->dst >= 0 && in->imm >= 0) {
          pt[in->dst] = (int)in->imm;
          mark_pt[in->dst] = (int)in->imm;
        } else if (in->op == NYIR_COPY && in->dst >= 0 && in->a >= 0 &&
                   (size_t)in->a < nv) {
          if (pt[in->a] >= 0)
            pt[in->dst] = pt[in->a];
          if (mark_pt[in->a] >= 0)
            mark_pt[in->dst] = mark_pt[in->a];
        } else if (in->op == NYIR_PHI && in->dst >= 0) {
          /*
           * A merged address may alias any mapped source; unmapped
           * (unknown) sources are conservatively assumed to alias them
           * too, except for null constants, which cannot address a
           * frame slot.  Rewrite provenance requires unanimity over
           * mapped sources with no unknown ones; escape marking takes
           * every mapped source whenever the merge is not provably
           * single-sourced.
           */
          int merged = -1;
          bool split = false;
          bool unknown = false;
          for (size_t k = 0; k < in->phi_incoming_len; ++k) {
            int vv = in->phi_incoming[k].value;
            if (vv < 0 || (size_t)vv >= nv) {
              unknown = true;
              continue;
            }
            if (pt[vv] < 0) {
              if (!sroa_value_is_null_const(f, vv))
                unknown = true;
              continue;
            }
            if (merged < 0) {
              merged = pt[vv];
            } else if (merged != pt[vv]) {
              split = true;
              break;
            }
          }
          if (!split && !unknown && merged >= 0) {
            pt[in->dst] = merged;
            mark_pt[in->dst] = merged;
          } else if (split || unknown) {
            for (size_t k = 0; k < in->phi_incoming_len; ++k) {
              int vv = in->phi_incoming[k].value;
              if (vv >= 0 && (size_t)vv < nv && mark_pt[vv] >= 0 &&
                  (size_t)mark_pt[vv] < slot_count)
                pt_escaped[mark_pt[vv]] = true;
            }
          }
        } else if ((in->op == NYIR_ADD_I64 || in->op == NYIR_SUB_I64) &&
                   in->dst >= 0 && in->a >= 0 && (size_t)in->a < nv &&
                   mark_pt[in->a] >= 0) {
          /*
           * Pointer arithmetic (including nonzero offsets) preserves
           * the base provenance for escape purposes only; pt[] is
           * deliberately untouched so rewriting never drops an offset.
           */
          mark_pt[in->dst] = mark_pt[in->a];
          bool second_tracked = in->b >= 0 && (size_t)in->b < nv &&
                                mark_pt[in->b] >= 0 &&
                                mark_pt[in->b] != mark_pt[in->a];
          if (second_tracked && (size_t)mark_pt[in->a] < slot_count &&
              (size_t)mark_pt[in->b] < slot_count) {
            /*
             * Sum of two differently-tracked pointers: the result may
             * alias either source.  Mark both now; the marking pass
             * below cannot see through the value.
             */
            pt_escaped[mark_pt[in->a]] = true;
            pt_escaped[mark_pt[in->b]] = true;
          }
        }
      }
      {
        for (size_t i = 0; i < f->len; ++i) {
          const nyr_inst_t *in = &f->data[i];
          if (in->op == NYIR_CALL) {
            int args[16];
            int argc = 0;
            if (!nyr_call_args(in, f->next_value, args, 16, &argc, NULL, 0))
              continue;
            for (int a = 0; a < argc; ++a) {
              if (args[a] >= 0 && (size_t)args[a] < nv &&
                  mark_pt[args[a]] >= 0 &&
                  (size_t)mark_pt[args[a]] < slot_count)
                pt_escaped[mark_pt[args[a]]] = true;
            }
          } else if (in->op == NYIR_RET && in->a >= 0 && (size_t)in->a < nv &&
                     mark_pt[in->a] >= 0 &&
                     (size_t)mark_pt[in->a] < slot_count) {
            /*
             * A returned derived pointer outlives the frame; the caller
             * may write through it, so the source slot is escaped even
             * when no call in this body observes it.
             */
            pt_escaped[mark_pt[in->a]] = true;
          } else if (in->op == NYIR_STORE_I64 && in->c >= 0 &&
                     (size_t)in->c < nv && mark_pt[in->c] >= 0 &&
                     (size_t)mark_pt[in->c] < slot_count) {
            /*
             * Storing a derived pointer as the *value* publishes it
             * through memory (unlike the address operand, which is just
             * used).  A later call may read it back and write through
             * it, so the source slot is escaped.
             */
            pt_escaped[mark_pt[in->c]] = true;
          }
        }
        for (size_t i = 0; i < f->len; ++i) {
          nyr_inst_t *in = &f->data[i];
          if (in->op == NYIR_LOAD_I64 && in->a >= 0 && (size_t)in->a < nv &&
              pt[in->a] >= 0 && (size_t)pt[in->a] < slot_count &&
              !pt_escaped[pt[in->a]] && in->dst >= 0) {
            nyr_range_t range = in->range;
            nyr_scalar_class_t scalar_class = in->scalar_class;
            uint8_t ownership = in->semantic_ownership;
            bool mutable_fact = in->semantic_mutable;
            nyr_debug_loc_t debug = in->debug;
            *in = (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                .dst = in->dst,
                                .a = -1,
                                .b = -1,
                                .imm = pt[in->a],
                                .scalar_class = scalar_class,
                                .semantic_ownership = ownership,
                                .semantic_mutable = mutable_fact,
                                .alias_class = 0,
                                .debug = debug,
                                .range = range};
          } else if (in->op == NYIR_STORE_I64 && in->a >= 0 &&
                     (size_t)in->a < nv && pt[in->a] >= 0 &&
                     (size_t)pt[in->a] < slot_count && !pt_escaped[pt[in->a]] &&
                     in->c >= 0) {
            uint8_t ownership = in->semantic_ownership;
            bool mutable_fact = in->semantic_mutable;
            nyr_debug_loc_t debug = in->debug;
            *in = (nyr_inst_t){.op = NYIR_STORE_LOCAL,
                                .dst = -1,
                                .a = in->c,
                                .b = -1,
                                .c = -1,
                                .imm = pt[in->a],
                                .semantic_ownership = ownership,
                                .semantic_mutable = mutable_fact,
                                .alias_class = 0,
                                .debug = debug};
          }
        }
        free(pt_escaped);
        free(pt);
        free(mark_pt);
      }
    }
  }

  /*
   * Tier 2: Shared Local-Escape & Access Analysis.
   */
  nyr_local_escape_info_t stk_escape[64] = {0};
  bool stk_loaded[64] = {0}, stk_stored[64] = {0};
  nyr_local_escape_info_t *escape =
      slot_count <= 64 ? stk_escape : calloc(slot_count, sizeof(*escape));
  bool *loaded =
      slot_count <= 64 ? stk_loaded : calloc(slot_count, sizeof(*loaded));
  bool *stored =
      slot_count <= 64 ? stk_stored : calloc(slot_count, sizeof(*stored));
  if (!escape || !loaded || !stored ||
      !nyr_analyze_local_escapes(f, escape, slot_count)) {
    if (slot_count > 64) {
      free(escape);
      free(loaded);
      free(stored);
    }
    return false;
  }

  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->imm < 0 || (size_t)in->imm >= slot_count)
      continue;
    if (in->op == NYIR_LOAD_LOCAL)
      loaded[in->imm] = true;
    else if (in->op == NYIR_STORE_LOCAL)
      stored[in->imm] = true;
  }

  /*
   * Dead stores to private, unread slots are unobservable.
   */
  for (size_t i = 0; i < f->len; ++i) {
    nyr_inst_t *in = &f->data[i];
    if (in->op == NYIR_STORE_LOCAL && in->imm >= 0 &&
        (size_t)in->imm < slot_count && !escape[in->imm].escapes &&
        !loaded[in->imm])
      nyr_inst_discard(in);
  }

  /*
   * Uninitialized private locals have the language-defined zero value.
   */
  for (size_t i = 0; i < f->len; ++i) {
    nyr_inst_t *in = &f->data[i];
    if (in->op == NYIR_LOAD_LOCAL && in->imm >= 0 &&
        (size_t)in->imm >= f->param_count && (size_t)in->imm < slot_count &&
        !escape[in->imm].escapes && !stored[in->imm]) {
      nir_make_const(in, 0);
    }
  }

  /*
   * Tier 3: Aggregate / Field Store Sinking & Scalar Replacement.
   */
  int stk_def[64], stk_last_store[64];
  int *current_def =
      slot_count <= 64 ? stk_def : malloc(slot_count * sizeof(*current_def));
  int *last_store = slot_count <= 64 ? stk_last_store
                                     : malloc(slot_count * sizeof(*last_store));
  if (!current_def || !last_store) {
    if (slot_count > 64) {
      free(escape);
      free(loaded);
      free(stored);
      free(current_def);
      free(last_store);
    }
    return false;
  }
  for (size_t i = 0; i < slot_count; ++i) {
    current_def[i] = -1;
    last_store[i] = -1;
  }

  bool any_replaced = false;
  for (size_t i = 0; i < f->len; ++i) {
    nyr_inst_t *in = &f->data[i];
    if (in->op == NYIR_LABEL || in->op == NYIR_BR || in->op == NYIR_BR_IF ||
        in->op == NYIR_RET) {
      for (size_t k = 0; k < slot_count; ++k) {
        current_def[k] = -1;
        last_store[k] = -1;
      }
      continue;
    }
    if (in->op == NYIR_CALL) {
      /*
       * A local value may be observed through aliases during a call.  The
       * escape summary only answers whether an address is retained; it does
       * not prove that a call cannot write through an address temporarily.
       * End all block-local forwarding facts at this memory boundary.
       */
      for (size_t k = 0; k < slot_count; ++k) {
        current_def[k] = -1;
        last_store[k] = -1;
      }
      continue;
    }
    if (in->op == NYIR_STORE_LOCAL && in->imm >= 0 &&
        (size_t)in->imm < slot_count && !escape[in->imm].escapes) {
      size_t slot = (size_t)in->imm;
      if (last_store[slot] >= 0) {
        size_t prev = (size_t)last_store[slot];
        nyr_inst_discard(&f->data[prev]);
        any_replaced = true;
      }
      last_store[slot] = (int)i;
      current_def[slot] = in->a;
    } else if (in->op == NYIR_LOAD_LOCAL && in->imm >= 0 &&
               (size_t)in->imm < slot_count && !escape[in->imm].escapes) {
      size_t slot = (size_t)in->imm;
      if (current_def[slot] >= 0 && in->dst >= 0) {
        nir_make_copy(in, current_def[slot]);
        last_store[slot] = -1; /* observed */
        any_replaced = true;
      }
    }
  }

  /*
   * Tier 4: Global Dead Store Cleanup.
   */
  if (any_replaced) {
    bool stk_ll[64] = {0};
    bool *local_loaded =
        slot_count <= 64 ? stk_ll : calloc(slot_count, sizeof(bool));
    if (local_loaded) {
      for (size_t i = 0; i < f->len; ++i)
        if (f->data[i].op == NYIR_LOAD_LOCAL && f->data[i].imm >= 0 &&
            (size_t)f->data[i].imm < slot_count)
          local_loaded[f->data[i].imm] = true;
      for (size_t i = 0; i < f->len; ++i) {
        nyr_inst_t *in = &f->data[i];
        if (in->op != NYIR_STORE_LOCAL || in->imm < 0 ||
            (size_t)in->imm >= slot_count || escape[in->imm].escapes)
          continue;
        /*
         * `local_loaded` is computed after scalar replacement, so a load
         * that was rewritten to a COPY disappears from that scan.  The
         * original `loaded` bitmap is the proof that the slot was observable;
         * dropping its initializing store leaves later CFG paths reading an
         * uninitialized local (notably ALLOCA-backed buffer pointers).
         */
        if (loaded[in->imm] || local_loaded[in->imm])
          continue;
        nyr_inst_discard(in);
      }
      if (slot_count > 64)
        free(local_loaded);
    }
    (void)nyr_compact_if_sparse(f);
  }

  if (slot_count > 64) {
    free(escape);
    free(loaded);
    free(stored);
    free(current_def);
    free(last_store);
  }
  return true;
}

bool nyr_sroa_scalar(nyr_func_t *f) { return nyr_sroa_unified(f); }
