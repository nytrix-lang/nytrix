/*
 * Reference C baseline for the optcheck integer kernel.
 * Expected output: 5168580067
 * Compile: cc -O3 -o integer-kernel integer-kernel.c && ./integer-kernel
 */
#include <stdio.h>
int main(void) {
  unsigned long long acc = 0, branch = 0;
  for (unsigned long long i = 1; i <= 250000ULL; ++i) {
      acc += (i % 97ULL) * (i % 89ULL);
      if (i % 11ULL == 0) branch += i;
  }
  unsigned long long state = 7, mixed = 0;
  for (unsigned long long i = 1; i <= 50000ULL; ++i) {
    state = (state * 1103515ULL + 12345ULL) % 2147483647ULL;
    if (state % 13ULL == 0) mixed += i;
  }
  long long signed_sum = 0;
  for (unsigned long long i = 1; i <= 100000ULL; ++i) {
    unsigned long long x = i * 1664525ULL + 1013904223ULL;
    if (x % 2ULL == 0) signed_sum += (long long)(x % 65537ULL);
    else signed_sum -= (long long)(x % 32749ULL);
  }
  printf("%lld\n", (long long)(acc + branch + mixed + state) + signed_sum);
  return 0;
}
