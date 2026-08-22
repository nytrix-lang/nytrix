/*
 * Dead-store elimination: removes memory stores to locations that
 * are never subsequently read before being overwritten or going dead.
 */
#include "code/native/ir/opt/util.h"
#include "code/native/ir/internal.h"
#include "base/compat.h"
#include "base/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Dead Store Elimination: remove STORE_LOCAL followed by another
 * STORE_LOCAL to the same slot without an intervening observation.
 *
 * Walk each basic block backwards.  This is deliberately linear in
 * the instruction stream: the previous implementation scanned ahead
 * from every store, which made store-heavy generated code quadratic.
 * Calls and control-flow boundaries remain conservative barriers.
 */

static int find_alloc_root_depth(const nyir_func_t *f, const int *defs, int v, int depth) {
  if (v < 0 || v >= f->next_value || depth > 16)
    return -1;
  int di = defs[v];
  if (di < 0 || (size_t)di >= f->len)
    return -1;
  const nyir_inst_t *in = &f->data[di];
  if (in->op == NYIR_COPY)
    return find_alloc_root_depth(f, defs, in->a, depth + 1);
  if (in->op == NYIR_ADD_I64) {
    int r = find_alloc_root_depth(f, defs, in->a, depth + 1);
    if (r >= 0)
      return r;
    return find_alloc_root_depth(f, defs, in->b, depth + 1);
  }
  if (in->op == NYIR_ALLOCA)
    return v;
  if (in->op == NYIR_CALL && in->symbol &&
      (strcmp(in->symbol, "rt_native_tbuf_new") == 0 ||
       strcmp(in->symbol, "malloc") == 0 ||
       strcmp(in->symbol, "calloc") == 0)) {
    return v;
  }
  return -1;
}

static int find_alloc_root(const nyir_func_t *f, const int *defs, int v) {
  return find_alloc_root_depth(f, defs, v, 0);
}

bool nyir_dead_store_elim(nyir_func_t *f) {
  if (!f || f->next_value <= 0)
    return true;
  size_t count = nyir_max_local(f);
  if (!count)
    return true;

  unsigned *pending_epoch = (unsigned *)calloc(count, sizeof(*pending_epoch));
  bool *observed = (bool *)calloc(count, sizeof(*observed));
  bool *escaped = (bool *)calloc(count, sizeof(*escaped));
  if (!pending_epoch || !observed || !escaped) {
    free(pending_epoch);
    free(observed);
    free(escaped);
    return false;
  }
  /*
   * A local has no externally visible identity unless its address is taken.
   * Once all direct loads have disappeared, every remaining store to it is
   * dead even across calls and CFG edges.
   */
  for (size_t i = 0; i < f->len; ++i) {
    const nyir_inst_t *in = &f->data[i];
    if ((in->op == NYIR_LOAD_LOCAL || in->op == NYIR_ADDR_LOCAL) &&
        in->imm >= 0 && (size_t)in->imm < count)
      observed[in->imm] = true;
    if (in->op == NYIR_ADDR_LOCAL && in->imm >= 0 &&
        (size_t)in->imm < count)
      escaped[in->imm] = true;
  }
  unsigned epoch = 1;
  for (size_t i = f->len; i-- > 0;) {
    nyir_inst_t *in = &f->data[i];
    if (in->op == NYIR_LABEL || in->op == NYIR_BR ||
        in->op == NYIR_BR_IF || in->op == NYIR_RET) {
      /*
       * Do not propagate facts across a control-flow boundary.
       */
      if (++epoch == 0) {
        memset(pending_epoch, 0, count * sizeof(*pending_epoch));
        epoch = 1;
      }
      continue;
    }
    /*
     * Calls cannot observe a local whose address is never materialized, so
     * direct local dead-store facts remain valid across calls. Address-taken
     * locals are excluded from this optimization below.
     */
    if (in->op == NYIR_CALL)
      continue;
    if (in->imm < 0 || (size_t)in->imm >= count)
      continue;
    size_t slot = (size_t)in->imm;
    if (in->op == NYIR_LOAD_LOCAL || in->op == NYIR_ADDR_LOCAL) {
      pending_epoch[slot] = 0;
      continue;
    }
    if (in->op != NYIR_STORE_LOCAL)
      continue;
    if (escaped[slot]) {
      pending_epoch[slot] = 0;
      continue;
    }
    if (!observed[slot]) {
      nyir_inst_discard(in);
      continue;
    }
    if (pending_epoch[slot] == epoch)
      nyir_inst_discard(in);
    pending_epoch[slot] = epoch;
  }
  int *defs = nyir_build_defs(f);
  if (defs) {
    bool *alloc_escapes = (bool *)calloc((size_t)f->next_value, sizeof(bool));
    bool *alloc_read = (bool *)calloc((size_t)f->next_value, sizeof(bool));
    if (alloc_escapes && alloc_read) {
      /*
       * Pass 1: find all reads, escapes, and parameter passes.
       */
      for (size_t i = 0; i < f->len; ++i) {
        const nyir_inst_t *in = &f->data[i];
        if (in->op == NYIR_LOAD_I64) {
          int root = find_alloc_root(f, defs, in->a);
          if (root >= 0 && root < f->next_value)
            alloc_read[root] = true;
        }
        if (in->op == NYIR_RET && in->a >= 0) {
          int root = find_alloc_root(f, defs, in->a);
          if (root >= 0 && root < f->next_value)
            alloc_escapes[root] = true;
        }
        if (in->op == NYIR_CALL) {
          int args[6] = {in->a, in->b, in->c, in->d, in->e, in->f};
          for (int k = 0; k < 6; ++k) {
            if (args[k] >= 0) {
              int root = find_alloc_root(f, defs, args[k]);
              if (root >= 0 && root < f->next_value && root != in->dst)
                alloc_escapes[root] = true;
            }
          }
        }
        if (in->op == NYIR_STORE_LOCAL && in->imm >= 0 && (size_t)in->imm < count) {
          int root = find_alloc_root(f, defs, in->a);
          if (root >= 0 && root < f->next_value) {
            if (escaped[in->imm] || observed[in->imm])
              alloc_read[root] = true;
            if (escaped[in->imm])
              alloc_escapes[root] = true;
          }
        }
      }
      /*
       * Pass 2: discard stores to dead allocations with no reads or escapes.
       */
      for (size_t i = 0; i < f->len; ++i) {
        nyir_inst_t *in = &f->data[i];
        if (in->op == NYIR_STORE_I64 && in->a >= 0) {
          int root = find_alloc_root(f, defs, in->a);
          if (root >= 0 && root < f->next_value && !alloc_escapes[root] && !alloc_read[root]) {
            nyir_inst_discard(in);
          }
        }
      }
    }
    free(alloc_escapes);
    free(alloc_read);
    free(defs);
  }
  free(pending_epoch);
  free(observed);
  free(escaped);
  return true;
}
