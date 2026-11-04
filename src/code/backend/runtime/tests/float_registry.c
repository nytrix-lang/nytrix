#include "code/backend/runtime/shared.h"

#include <stdio.h>
#include <stdlib.h>

int main(void) {
  /*
   * The old fixed registry silently stopped registering at 2^20 live values.
   * UI frames can exceed that limit without retaining a large Ny container.
   */
  const size_t count = (1u << 20) + 4096;
  int64_t *values = malloc(count * sizeof(*values));
  if (!values)
    return 1;
  for (size_t i = 0; i < count; ++i) {
    values[i] = rt_flt_box_double((double)i + 0.25);
    if (!values[i] || !rt_float_registry_has((uintptr_t)values[i])) {
      fprintf(stderr, "float provenance lost at allocation %zu\n", i);
      return 1;
    }
  }
  for (size_t i = 0; i < count; ++i) {
    if (!rt_float_registry_has((uintptr_t)values[i]) ||
        rt_flt_unbox_double(values[i]) != (double)i + 0.25) {
      fprintf(stderr, "float value lost after registry growth at %zu\n", i);
      return 1;
    }
  }
  if (rt_float_registry_has(8))
    return 1;
  for (size_t i = 0; i < count; ++i) {
    rt_flt_free(values[i]);
    if (rt_float_registry_has((uintptr_t)values[i])) {
      fputs("freed float still registered\n", stderr);
      return 1;
    }
  }
  /*
   * Reuse pooled slots after removal and tombstone cleanup.
   */
  for (size_t i = 0; i < count; ++i) {
    values[i] = rt_flt_box_double(-(double)i - 0.5);
    if (!rt_float_registry_has((uintptr_t)values[i]) ||
        rt_flt_unbox_double(values[i]) != -(double)i - 0.5)
      return 1;
  }
  for (size_t i = 0; i < count; ++i)
    rt_flt_free(values[i]);
  free(values);
  puts("FLOAT-REGISTRY-GROWTH-OK");
  return 0;
}
