/*
 * Regression test for deduplicating ranges in the runtime map oracle.
 */
#include "code/backend/runtime/shared.h"
#include <stdio.h>
#include <stdlib.h>

int main(void) {
#ifdef __linux__
  enum { RANGES = 32, PAGE_BYTES = 4096 };
  void *ranges[RANGES];
  uint32_t before =
      atomic_load_explicit(&g_rt_oracle_extra_count, memory_order_acquire);
  for (size_t i = 0; i < RANGES; ++i) {
    ranges[i] = aligned_alloc(PAGE_BYTES, PAGE_BYTES);
    if (!ranges[i])
      return 1;
  }
  for (size_t pass = 0; pass < 32; ++pass)
    for (size_t i = 0; i < RANGES; ++i)
      rt_map_oracle_add((uintptr_t)ranges[i], PAGE_BYTES);
  uint32_t after =
      atomic_load_explicit(&g_rt_oracle_extra_count, memory_order_acquire);
  int failed = after > before + RANGES ||
               atomic_load_explicit(&g_rt_oracle_full, memory_order_acquire);
  for (size_t i = 0; i < RANGES; ++i)
    free(ranges[i]);
  if (!failed) {
    atomic_store_explicit(&g_rt_oracle_ready, 1, memory_order_release);
    g_rt_oracle_snapshot_count = 0;
    atomic_store_explicit(&g_rt_oracle_lo, UINTPTR_MAX, memory_order_release);
    atomic_store_explicit(&g_rt_oracle_hi, 0, memory_order_release);
    atomic_store_explicit(&g_rt_oracle_full, 1, memory_order_release);
    const uintptr_t registered = UINT64_C(0x600000000000);
    const uintptr_t distant = UINT64_C(0x500000000000);
    rt_map_oracle_add(registered, PAGE_BYTES);
    int bounds_cover_range =
        atomic_load_explicit(&g_rt_oracle_lo, memory_order_acquire) <=
            registered &&
        atomic_load_explicit(&g_rt_oracle_hi, memory_order_acquire) >=
            registered + PAGE_BYTES;
    int distant_rejected =
        rt_map_oracle_check(distant, sizeof(uintptr_t)) == -1;
    if (!bounds_cover_range || !distant_rejected) {
      fprintf(stderr,
              "full oracle lost conservative rejection: bounds=%d reject=%d\n",
              bounds_cover_range, distant_rejected);
      return 3;
    }
  }
  if (failed) {
    fprintf(stderr, "duplicate mappings exhausted oracle: before=%u after=%u\n",
            before, after);
    return 2;
  }
#endif
  puts("MAP-ORACLE-DEDUP-OK");
  return 0;
}
