/*
 * C frontend test fixture: aggregate variadic va arg.
 */
#include "aggregate-variadic-va-arg.h"

int main(void) {
  struct va_uid uid = {42};
  struct va_real real = {1.25};
  struct va_pair pair = {11, 13};
  struct va_reals reals = {2.25, 3.5};
  struct va_mixed mixed = {43, 4.5};
  struct va_reverse reverse = {5.25, 53};
  struct va_aligned aligned = {67, 71, 73};
  struct va_wide wide = {79, 83, 89};
  if (!va_registers(7, uid, real, pair, reals, mixed, reverse, 59UL))
    return 1;
  if (!va_gp_overflow(0, 1UL, 2UL, 3UL, 4UL, pair, 17UL, mixed, 6.25))
    return 2;
  if (!va_sse_overflow(0, 1., 2., 3., 4., 5., 6., 7., reals, 6.25, reverse,
                       59UL))
    return 3;
  if (!va_memory(0, 1UL, 2UL, 3UL, 4UL, 5UL, 61UL, aligned, wide, 97UL))
    return 4;
  return 0;
}
