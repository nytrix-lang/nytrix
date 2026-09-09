/*
 * cc -std=gnu11 -D_GNU_SOURCE -Isrc etc/tests/native/runtime/callable-tags.c \
 *   -o /tmp/nytrix-callable-tags && /tmp/nytrix-callable-tags
 */
#include "code/runtime/shared.h"
#include <assert.h>

int main(void) {
  const int64_t edges[] = {INT64_MIN / 2, INT64_MIN / 2 + 1,
                           -2305843009213693952LL, -1025, -9, -8, -7, -6,
                           -5, -4, -3, -2, -1, 0, 1, 2, 3, 4,
                           2305843009213693951LL, INT64_MAX / 2};
  for (size_t i = 0; i < sizeof(edges) / sizeof(edges[0]); ++i) {
    int64_t value = rt_tag_v(edges[i]);
    assert(is_int(value));
    assert(!NY_DYNAMIC_CALLABLE_IS(value));
    assert(!NY_NATIVE_IS(value));
    assert(rt_untag_v(value) == edges[i]);
  }
  /*
   * Code addresses need not be aligned; preserve every low address bit.
   */
  for (uintptr_t address = 0x10000; address < 0x10020; ++address) {
    void *code = (void *)address;
    int64_t dynamic = NY_DYNAMIC_CALLABLE_ENCODE(code);
    int64_t predicate = NY_DYNAMIC_CALLABLE_BOOL_ENCODE(code);
    assert(NY_DYNAMIC_CALLABLE_IS(dynamic));
    assert(!NY_DYNAMIC_CALLABLE_BOOL_IS(dynamic));
    assert(NY_DYNAMIC_CALLABLE_BOOL_IS(predicate));
    assert(!is_int(dynamic) && !is_int(predicate));
    assert(!is_ptr(dynamic) && !is_ptr(predicate));
    assert(!NY_NATIVE_IS(dynamic) && !NY_NATIVE_IS(predicate));
    assert(NY_DYNAMIC_CALLABLE_DECODE(dynamic) == code);
    assert(NY_DYNAMIC_CALLABLE_DECODE(predicate) == code);
    assert(rt_untag_v(predicate) == (int64_t)address);
  }
  puts("callable tags: integers and pointers remain disjoint");
  return 0;
}
