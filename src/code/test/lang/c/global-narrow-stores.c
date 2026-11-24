/* Stores to narrow file-scope objects write only their own bytes, and float globals round-trip; neighbouring objects stay intact. */
#include <stdio.h>
static short a = 1;
static unsigned short b = 7;
static signed char c = 1;
static unsigned char d = 9;
static float f = 1.5f;
static float h = 2.5f;
static char e = 3;
static int i32 = 4;
static unsigned char u8 = 5;
float k;
__attribute__((noinline)) void wr(int v) {
  a = v; c = v; f = v; e = v; i32 = v; u8 = v;
  a += 1; c -= 1; e ^= 3; u8 |= 1; b = v; k = f * 2.0f; h = 3.0;
}
int main(void) {
  wr(300);
  printf("%d %d %d %d %g %g %d %d %d %g\n", a, b, c, d, (double)f, (double)h, e, i32, u8, (double)k);
  return 0;
}
