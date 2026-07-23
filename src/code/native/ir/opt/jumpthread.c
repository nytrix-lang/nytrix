#include "code/native/ir/opt/util.h"
#include "code/native/ir/internal.h"
#include "base/compat.h"
#include "base/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Jump Threading: redirect BR → LABEL that only does BR(target)      */
/*                                                                    */
/* If LABEL(L) is immediately followed by an unconditional BR(T),     */
/* then any BR_IF(L) or BR(L) can be redirected to T instead.  This   */
/* eliminates dead fallthrough labels and is a key LLVM peephole.     */
/* ------------------------------------------------------------------ */

typedef struct {
  int64_t label;
  int64_t target;
} nyir_jump_redirect_t;

static int nyir_jump_redirect_cmp(const void *lhs, const void *rhs) {
  const nyir_jump_redirect_t *a = lhs;
  const nyir_jump_redirect_t *b = rhs;
  return (a->label > b->label) - (a->label < b->label);
}

static size_t nyir_jump_redirect_find(const nyir_jump_redirect_t *entries,
                                        size_t count, int64_t label) {
  size_t first = 0;
  size_t last = count;
  while (first < last) {
    size_t mid = first + (last - first) / 2;
    if (entries[mid].label == label)
      return mid;
    if (entries[mid].label < label)
      first = mid + 1;
    else
      last = mid;
  }
  return SIZE_MAX;
}

bool nyir_jump_thread(nyir_func_t *f) {
  if (!f || f->len < 3)
    return true;
  /* Labels are IDs, not dense array indices. Keep only forwarding labels so
   * generated functions with sparse or very large IDs retain jump threading. */
  nyir_jump_redirect_t *redirect =
      calloc(f->len, sizeof(*redirect));
  if (!redirect)
    return false;

  /* For each LABEL followed directly by BR(target), record redirect. */
  size_t redirect_count = 0;
  for (size_t i = 0; i + 1 < f->len; ++i) {
    if (f->data[i].op != NYIR_LABEL || f->data[i].imm < 0)
      continue;
    size_t next = nir_next_non_nop(f, i + 1);
    if (next < f->len && f->data[next].op == NYIR_BR && f->data[next].imm >= 0) {
      redirect[redirect_count++] = (nyir_jump_redirect_t){
          .label = f->data[i].imm,
          .target = f->data[next].imm,
      };
    }
  }
  if (redirect_count == 0) {
    free(redirect);
    return true;
  }
  qsort(redirect, redirect_count, sizeof(*redirect), nyir_jump_redirect_cmp);

  /* Chase forwarding chains. The cap is the number of forwarding labels, so
   * cycles remain unchanged without imposing an arbitrary nesting limit. */
  unsigned *seen = calloc(redirect_count, sizeof(*seen));
  if (!seen) {
    free(redirect);
    return false;
  }
  unsigned epoch = 1;
  for (size_t i = 0; i < redirect_count; ++i) {
    if (++epoch == 0) {
      memset(seen, 0, redirect_count * sizeof(*seen));
      epoch = 1;
    }
    int64_t original = redirect[i].target;
    int64_t target = original;
    bool cycle = false;
    seen[i] = epoch;
    for (size_t hops = 0; hops < redirect_count; ++hops) {
      size_t next = nyir_jump_redirect_find(redirect, redirect_count, target);
      if (next == SIZE_MAX)
        break;
      if (seen[next] == epoch) {
        cycle = true;
        break;
      }
      seen[next] = epoch;
      target = redirect[next].target;
    }
    if (!cycle)
      redirect[i].target = target;
    else
      redirect[i].target = original;
  }

  /* Rewrite BR and BR_IF targets. */
  for (size_t i = 0; i < f->len; ++i) {
    nyir_inst_t *in = &f->data[i];
    if (in->op != NYIR_BR && in->op != NYIR_BR_IF)
      continue;
    size_t entry =
        in->imm >= 0 ? nyir_jump_redirect_find(redirect, redirect_count,
                                                  in->imm)
                     : SIZE_MAX;
    if (entry != SIZE_MAX)
      in->imm = redirect[entry].target;
  }

  free(seen);
  free(redirect);
  return true;
}
