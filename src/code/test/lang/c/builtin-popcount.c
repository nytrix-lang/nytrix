/*
 * __builtin_popcount, popcountl and popcountll agree with GCC for edge and mixed bit patterns.
 */
#include <stdint.h>
#include <stdio.h>
__attribute__((noinline)) int pc32(unsigned x) { return __builtin_popcount(x); }
__attribute__((noinline)) int pcl(unsigned long x) { return __builtin_popcountl(x); }
__attribute__((noinline)) int pcll(unsigned long long x) { return __builtin_popcountll(x); }
int main(void) {
  unsigned long long v[] = {0ull, 1ull, 0xffull, 0x80000000ull, 0xffffffffull, 0x100000000ull,
                            0x8000000000000000ull, 0xffffffffffffffffull, 0x5555555555555555ull,
                            0xaaaaaaaaaaaaaaaaull, 0x0123456789abcdefull, 0xfedcba9876543210ull};
  unsigned sum = 0;
  for (unsigned i = 0; i < sizeof(v) / sizeof(v[0]); ++i) {
    printf("%d %d %d\n", pc32((unsigned)v[i]), pcl((unsigned long)v[i]), pcll(v[i]));
    sum += (unsigned)__builtin_popcountll(v[i] * 2654435761ull);
  }
  printf("%u %d %d\n", sum, __builtin_popcount(0xf0f0u), __builtin_popcountll(~0ull) + 1);
  return 0;
}
