/*
 * Regression test for recognizing valid small-string storage slots.
 */
#include "code/backend/runtime/shared.h"
#include <stdint.h>
#include <stdio.h>

int main(void) {
  int64_t first = rt_alloc_small_string_len("a", 1);
  int64_t second = rt_alloc_small_string_len("b", 1);
  if (!first || !second || !rt_is_sso_string(first) ||
      !rt_is_sso_string(second)) {
    fprintf(stderr, "allocated SSO strings were not recognized\n");
    return 1;
  }

  uintptr_t stride = (uintptr_t)second - (uintptr_t)first;
  uintptr_t next_slot = (uintptr_t)second + stride;
  if (!stride || rt_is_sso_string((int64_t)next_slot)) {
    fprintf(stderr, "unused SSO slot was classified as a string\n");
    return 2;
  }
  int64_t third = rt_alloc_small_string_len("c", 1);
  if ((uintptr_t)third != next_slot || !rt_is_sso_string(third)) {
    fprintf(stderr, "SSO allocation did not invalidate a cached negative\n");
    return 3;
  }

  puts("SSO-PROBE-CACHE-OK");
  rt_cleanup_small_strings();
  return 0;
}
