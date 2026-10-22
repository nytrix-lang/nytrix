#include "src/code/lang/c/tests/win64-ny-aggregate-host.h"
#include "etc/tests/lang/interop/c/internal-byvalue-param-import.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

extern int64_t ny_fn_aggregate_argument_probe(SumPairLayout value);

void *rt_zalloc_raw(uint64_t size) { return calloc(1, (size_t)size); }

int64_t rt_cstr_to_str(int64_t value) { return value; }

int64_t rt_assert_cstr(int64_t condition, int64_t message) {
  if (condition)
    return 0;
  fprintf(stderr, "%s\n", (const char *)(uintptr_t)message);
  abort();
}

SumPairLayout make_sum_pair(void) {
  SumPairLayout value = {10, 11, 12};
  return value;
}

long sum_pair(SumPairLayout value) { return value.a + value.b + value.c; }

long check_aggregate_argument(void) {
  SumPairLayout value = {10, 11, 12};
  return (long)ny_fn_aggregate_argument_probe(value);
}
