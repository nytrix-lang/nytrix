/*
 * Unified Multi-Tier SROA Engine:
 * Combines points-to promotion, escape analysis, dead store elimination,
 * and scalar/aggregate replacement into a single cohesive pipeline with
 * shared local escape analysis and minimal graph rescanning.
 */
#include "code/ir/opt/util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool nyir_sroa_unified(nyir_func_t *f);
bool nyir_sroa_scalar(nyir_func_t *f);
bool nyir_escape_sroa(nyir_func_t *f);

bool nyir_sroa_unified(nyir_func_t *f) {
  if (!f || f->len == 0)
    return true;

  size_t slot_count = nyir_local_slot_count(f);
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
    if (pt) {
      for (size_t i = 0; i < nv; ++i)
        pt[i] = -1;
      for (size_t i = 0; i < f->len; ++i) {
        const nyir_inst_t *in = &f->data[i];
        if (in->op == NYIR_ADDR_LOCAL && in->dst >= 0 && in->imm >= 0) {
          pt[in->dst] = (int)in->imm;
        } else if (in->op == NYIR_COPY && in->dst >= 0 && in->a >= 0 &&
                   (size_t)in->a < nv && pt[in->a] >= 0) {
          pt[in->dst] = pt[in->a];
        }
      }
      bool *pt_escaped = calloc(slot_count, sizeof(bool));
      if (pt_escaped) {
        for (size_t i = 0; i < f->len; ++i) {
          const nyir_inst_t *in = &f->data[i];
          if (in->op != NYIR_CALL)
            continue;
          int args[16];
          int argc = 0;
          if (!nyir_call_args(in, f->next_value, args, 16, &argc, NULL, 0))
            continue;
          for (int a = 0; a < argc; ++a) {
            if (args[a] >= 0 && (size_t)args[a] < nv && pt[args[a]] >= 0 &&
                (size_t)pt[args[a]] < slot_count)
              pt_escaped[pt[args[a]]] = true;
          }
        }
        for (size_t i = 0; i < f->len; ++i) {
          nyir_inst_t *in = &f->data[i];
          if (in->op == NYIR_LOAD_I64 && in->a >= 0 && (size_t)in->a < nv &&
              pt[in->a] >= 0 && (size_t)pt[in->a] < slot_count &&
              !pt_escaped[pt[in->a]] && in->dst >= 0) {
            *in = (nyir_inst_t){.op = NYIR_LOAD_LOCAL,
                                .dst = in->dst,
                                .a = -1,
                                .b = -1,
                                .imm = pt[in->a]};
          } else if (in->op == NYIR_STORE_I64 && in->a >= 0 && (size_t)in->a < nv &&
                     pt[in->a] >= 0 && (size_t)pt[in->a] < slot_count &&
                     !pt_escaped[pt[in->a]] && in->c >= 0) {
            *in = (nyir_inst_t){.op = NYIR_STORE_LOCAL,
                                .dst = -1,
                                .a = in->c,
                                .b = -1,
                                .c = -1,
                                .imm = pt[in->a]};
          }
        }
        free(pt_escaped);
      }
      free(pt);
    }
  }

  /*
   * Tier 2: Shared Local-Escape & Access Analysis.
   */
  nyir_local_escape_info_t stk_escape[64] = {0};
  bool stk_loaded[64] = {0}, stk_stored[64] = {0};
  nyir_local_escape_info_t *escape = slot_count <= 64 ? stk_escape : calloc(slot_count, sizeof(*escape));
  bool *loaded = slot_count <= 64 ? stk_loaded : calloc(slot_count, sizeof(*loaded));
  bool *stored = slot_count <= 64 ? stk_stored : calloc(slot_count, sizeof(*stored));
  if (!escape || !loaded || !stored ||
      !nyir_analyze_local_escapes(f, escape, slot_count)) {
    if (slot_count > 64) { free(escape); free(loaded); free(stored); }
    return false;
  }

  for (size_t i = 0; i < f->len; ++i) {
    const nyir_inst_t *in = &f->data[i];
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
    nyir_inst_t *in = &f->data[i];
    if (in->op == NYIR_STORE_LOCAL && in->imm >= 0 &&
        (size_t)in->imm < slot_count && !escape[in->imm].escapes &&
        !loaded[in->imm])
      *in = (nyir_inst_t){.op = NYIR_NOP, .dst = -1, .a = -1, .b = -1};
  }

  /*
   * Uninitialized private locals have the language-defined zero value.
   */
  for (size_t i = 0; i < f->len; ++i) {
    nyir_inst_t *in = &f->data[i];
    if (in->op == NYIR_LOAD_LOCAL && in->imm >= 0 &&
        (size_t)in->imm >= f->param_count &&
        (size_t)in->imm < slot_count && !escape[in->imm].escapes &&
        !stored[in->imm])
      *in = (nyir_inst_t){.op = NYIR_CONST_I64, .dst = in->dst, .imm = 0};
  }

  /*
   * Tier 3: Aggregate / Field Store Sinking & Scalar Replacement.
   */
  int stk_def[64], stk_last_store[64];
  int *current_def = slot_count <= 64 ? stk_def : malloc(slot_count * sizeof(*current_def));
  int *last_store = slot_count <= 64 ? stk_last_store : malloc(slot_count * sizeof(*last_store));
  if (!current_def || !last_store) {
    if (slot_count > 64) { free(escape); free(loaded); free(stored); free(current_def); free(last_store); }
    return false;
  }
  for (size_t i = 0; i < slot_count; ++i) {
    current_def[i] = -1;
    last_store[i] = -1;
  }

  bool any_replaced = false;
  for (size_t i = 0; i < f->len; ++i) {
    nyir_inst_t *in = &f->data[i];
    if (in->op == NYIR_LABEL || in->op == NYIR_BR ||
        in->op == NYIR_BR_IF || in->op == NYIR_RET) {
      for (size_t k = 0; k < slot_count; ++k) {
        current_def[k] = -1;
        last_store[k] = -1;
      }
      continue;
    }
    if (in->op == NYIR_CALL) {
      for (size_t k = 0; k < slot_count; ++k) {
        if (escape[k].escapes) {
          current_def[k] = -1;
          last_store[k] = -1;
        }
      }
      continue;
    }
    if (in->op == NYIR_STORE_LOCAL && in->imm >= 0 &&
        (size_t)in->imm < slot_count && !escape[in->imm].escapes) {
      size_t slot = (size_t)in->imm;
      if (last_store[slot] >= 0) {
        size_t prev = (size_t)last_store[slot];
        f->data[prev] = (nyir_inst_t){.op = NYIR_NOP, .dst = -1, .a = -1, .b = -1};
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
    bool *local_loaded = slot_count <= 64 ? stk_ll : calloc(slot_count, sizeof(bool));
    if (local_loaded) {
      for (size_t i = 0; i < f->len; ++i)
        if (f->data[i].op == NYIR_LOAD_LOCAL && f->data[i].imm >= 0 &&
            (size_t)f->data[i].imm < slot_count)
          local_loaded[f->data[i].imm] = true;
      for (size_t i = 0; i < f->len; ++i) {
        nyir_inst_t *in = &f->data[i];
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
        *in = (nyir_inst_t){.op = NYIR_NOP, .dst = -1, .a = -1, .b = -1,
                            .c = -1, .d = -1, .e = -1, .f = -1};
      }
      if (slot_count > 64) free(local_loaded);
    }
    (void)nyir_compact_if_sparse(f);
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

bool nyir_sroa_scalar(nyir_func_t *f) {
  return nyir_sroa_unified(f);
}
