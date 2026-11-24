/* Loop-invariant loads and branches in a loop with a dead result keep every operand defined, at -O3. */
#include <stdint.h>
#include <stdio.h>
unsigned g5 = 7, g2 = 11, g0 = 13;
int a5 = 3, a0 = 4;
__attribute__((noinline)) unsigned long diamond(long a) {
  for (int i = 0; i < 5; ++i) {
    unsigned long x = a != 25 ? g2 : g0;
    unsigned long y = g5 % 1259646381ul;
    (void)x;
    (void)y;
  }
  return g5 + (unsigned long)a;
}
__attribute__((noinline)) int select_loop(void) {
  for (int i = 0; i < 3; ++i) {
    int e = a5 < a0 ? a5 : a0;
    (void)e;
  }
  return a5 + a0;
}
int main(void) {
  printf("%lu %lu %d\n", diamond(25), diamond(1), select_loop());
  return 0;
}
