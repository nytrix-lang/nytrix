/*
 * Reference C baseline for the optcheck floating-point kernel.
 * Expected output: 4
 * Compile: cc -O3 -o floating-kernel floating-kernel.c && ./floating-kernel
 */
#include <stdio.h>
int main(void) {
  double a = 1.5, b = 0.75;
  unsigned long long hits = 0;
  for (unsigned long long i = 1; i <= 4ULL; ++i) {
    a = a + b * 0.000001;
    b = b + a * 0.0000005;
    if (a > b) ++hits;
  }
  printf("%llu\n", hits);
  return 0;
}
