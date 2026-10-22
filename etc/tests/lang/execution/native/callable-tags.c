/*
 * cc -std=gnu11 -D_GNU_SOURCE -ffunction-sections -fdata-sections -Isrc \
 *   etc/tests/lang/execution/native/callable-tags.c
 * src/code/lang/runtime/ffi.c \
 *   -Wl,--gc-sections -ldl -pthread -o /tmp/nytrix-callable-tags && \
 *   /tmp/nytrix-callable-tags
 */
#include "code/runtime/shared.h"
#include <assert.h>

int main(void) {
  const int64_t edges[] = {INT64_MIN / 2,
                           INT64_MIN / 2 + 1,
                           -2305843009213693952LL,
                           -1025,
                           -9,
                           -8,
                           -7,
                           -6,
                           -5,
                           -4,
                           -3,
                           -2,
                           -1,
                           0,
                           1,
                           2,
                           3,
                           4,
                           2305843009213693951LL,
                           INT64_MAX / 2};
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
#if UINTPTR_MAX > UINT32_MAX
  /*
   * ABI flag bits must never overwrite high code-address bits.
   */
  void *wide_code = (void *)(uintptr_t)((UINT64_C(1) << 60) | 0x12340);
  int64_t wide_dynamic = NY_DYNAMIC_CALLABLE_ENCODE(wide_code);
  int64_t wide_predicate = NY_DYNAMIC_CALLABLE_BOOL_ENCODE(wide_code);
  assert(NY_DYNAMIC_CALLABLE_DECODE(wide_dynamic) == wide_code);
  assert(NY_DYNAMIC_CALLABLE_DECODE(wide_predicate) == wide_code);
  assert(NY_DYNAMIC_CALLABLE_BOOL_IS(wide_predicate));
#endif
  puts("callable tags: integers and pointers remain disjoint");
  return 0;
}
