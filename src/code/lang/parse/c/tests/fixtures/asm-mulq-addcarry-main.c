#include <stdint.h>

unsigned long long nyc_mulq_addcarry(unsigned long long low,
                                     unsigned long long multiplier,
                                     unsigned long long addend,
                                     unsigned long long *high_out);
unsigned long long nyc_divq(unsigned long long low,
                            unsigned long long high,
                            unsigned long long divisor,
                            unsigned long long *remainder_out);

int main(void) {
  unsigned long long high = 0;
  unsigned long long low = nyc_mulq_addcarry(UINT64_MAX - 1, 2, 3, &high);
  if (low != UINT64_MAX || high != 1)
    return 1;
  unsigned long long remainder = 0;
  low = nyc_divq(0, 1, 3, &remainder);
  return low == 6148914691236517205ULL && remainder == 1 ? 0 : 2;
}
