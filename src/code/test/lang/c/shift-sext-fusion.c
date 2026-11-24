/* A left shift by 32 before a sign-extending narrowing store is not folded away, on every route and level. */
#include <stdint.h>
#include <stdio.h>
static int32_t g6 = 1167098989;
static int32_t g7 = 77;
static int16_t g8 = 9;
__attribute__((noinline)) void narrow(uint64_t x) {
  g6 = (int64_t)(x << 32);
  g7 = (int64_t)(x << 33 | 5);
  g8 = (int64_t)(x << 48 | 3);
}
int main(void) {
  narrow(56);
  printf("%d %d %d\n", g6, g7, g8);
  narrow(0x1234567812345678ull);
  printf("%d %d %d\n", g6, g7, g8);
  return 0;
}
