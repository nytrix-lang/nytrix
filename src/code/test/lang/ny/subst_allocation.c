/*
 * Exercise failure after each independently moving substitution-table grow.
 */
#include "code/lang/ny/semantics/typeinfer.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ny_subst_t *active;
static int calls;
static int fail_at;

void *__real_realloc(void *ptr, size_t bytes);
void *__wrap_realloc(void *ptr, size_t bytes);
void *__wrap_realloc(void *ptr, size_t bytes) {
  if (!active)
    return __real_realloc(ptr, bytes);
  if (++calls == fail_at)
    return NULL;
  size_t old_bytes = 0;
  if (ptr == active->parent || ptr == active->rank)
    old_bytes = (size_t)active->capacity * sizeof(int);
  else if (ptr == active->binding)
    old_bytes = (size_t)active->capacity * sizeof(ny_type_t *);
  else
    abort();
  void *replacement = malloc(bytes);
  assert(replacement);
  memcpy(replacement, ptr, old_bytes < bytes ? old_bytes : bytes);
  free(ptr);
  return replacement;
}

int main(void) {
  typeinfer_ctx_t ctx;
  typeinfer_ctx_init(&ctx, SIZE_MAX, NULL, NULL);
  assert(!ctx.formal_hm_enabled && !ctx.vars && !ctx.hash_table &&
         !ctx.subst);
  typeinfer_ctx_dispose(&ctx);

  for (int failure = 1; failure <= 3; failure++) {
    ny_subst_t *s = ny_subst_new(2);
    assert(s && ny_subst_fresh(s) == 0 && ny_subst_fresh(s) == 1);
    ny_type_t sentinel = {0};
    s->parent[1] = 0;
    s->rank[0] = 1;
    s->binding[0] = &sentinel;
    active = s;
    fail_at = failure;
    calls = 0;
    assert(ny_subst_fresh(s) == -1);
    assert(s->count == 2 && s->capacity == 2);
    assert(s->parent[1] == 0 && s->rank[0] == 1 && s->binding[0] == &sentinel);
    /*
     * A caller may retry after OOM; every successful realloc stays owned.
     */
    fail_at = 0;
    calls = 0;
    assert(ny_subst_fresh(s) == 2 && s->capacity == 4);
    assert(s->parent[1] == 0 && s->rank[0] == 1 && s->binding[0] == &sentinel);
    assert(s->parent[2] == 2 && s->rank[2] == 0 && !s->binding[2]);
    active = NULL;
    ny_subst_free(s);
  }
  ny_subst_t huge = {.count = INT_MAX, .capacity = INT_MAX};
  assert(ny_subst_fresh(&huge) == -1);
  puts("substitution allocation failure preserves state and retry");
  return 0;
}
