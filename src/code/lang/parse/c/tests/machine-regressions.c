/*
 * Encoder, allocator, and optimizer regressions found through the C machine
 * path. Exits with the number of the first failing check, or 0.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(id, cond)                                                        \
  do {                                                                         \
    if (!(cond)) {                                                             \
      printf("check %d failed\n", id);                                         \
      return id;                                                               \
    }                                                                          \
  } while (0)

static uint64_t opaque(uint64_t v) {
  volatile uint64_t x = v;
  return x;
}

/*
 * A recursive result added into the loop's merged accumulator.
 */
static int queens(int n, int row, int cols, int d1, int d2) {
  if (row == n)
    return 1;
  int count = 0;
  for (int i = 0; i < n; i++) {
    int c = 1 << i, a = 1 << (row + i), b = 1 << (row - i + n);
    if ((cols & c) == 0 && (d1 & a) == 0 && (d2 & b) == 0)
      count += queens(n, row + 1, cols | c, d1 | a, d2 | b);
  }
  return count;
}

/*
 * The loop counter is merged with its increment: the home must be written.
 */
static int collatz_best(int limit) {
  int best = 0;
  for (int value = 1; value < limit; value++) {
    long n = value;
    int count = 0;
    while (n != 1) {
      n = (n & 1) ? 3 * n + 1 : n / 2;
      ++count;
    }
    if (count > best)
      best = count;
  }
  return best;
}

static uint32_t rotr(uint32_t x, uint32_t n) {
  return (x >> n) | (x << (32 - n));
}

/*
 * Indexed reads whose inputs die before the access, under register pressure.
 */
static uint32_t schedule(const uint8_t *buf) {
  uint32_t w[64];
  for (int i = 0; i < 16; i++)
    w[i] = ((uint32_t)buf[i * 4] << 24) | ((uint32_t)buf[i * 4 + 1] << 16) |
           ((uint32_t)buf[i * 4 + 2] << 8) | (uint32_t)buf[i * 4 + 3];
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t sum = 0;
  for (int i = 0; i < 64; i++)
    sum = rotr(sum, 5) ^ w[i];
  return sum;
}

int main(int argc, char **argv) {
  (void)argv;
  CHECK(1, argc == 1);

  uint64_t big = opaque(UINT64_C(0xF000000000000010));
  CHECK(2, big >> opaque(4) == UINT64_C(0x0F00000000000001));
  CHECK(3, big / opaque(16) == UINT64_C(0x0F00000000000001));
  CHECK(4, big % opaque(7) == UINT64_C(0xF000000000000010) % 7);

  volatile uint16_t h[3] = {0xFFFEu, 1, 2};
  volatile int32_t s[3] = {-5, 7, 9};
  CHECK(5, h[0] == 0xFFFEu && h[1] == 1);
  CHECK(6, s[0] == -5 && (int64_t)s[0] < 0);
  h[1] = 0xABCDu;
  s[1] = -1;
  CHECK(7, h[0] == 0xFFFEu && h[1] == 0xABCDu && h[2] == 2);
  CHECK(8, s[0] == -5 && s[1] == -1 && s[2] == 9);

  uint8_t out[32], u[32];
  for (int i = 0; i < 32; i++) {
    out[i] = (uint8_t)(i * 3 + 1);
    u[i] = (uint8_t)(i * 7 + 5);
  }
  for (int i = 0; i < 32; i++)
    out[i] ^= u[i];
  for (int i = 0; i < 32; i++)
    CHECK(9, out[i] == (uint8_t)((i * 3 + 1) ^ (i * 7 + 5)));

  CHECK(10, queens(6, 0, 0, 0, 0) == 4);
  CHECK(11, collatz_best(100) == 118);

  uint8_t buf[64];
  for (int i = 0; i < 64; i++)
    buf[i] = (uint8_t)(i * 11 + 3);
  CHECK(12, schedule(buf) == 334689215u);
  /*
   * Unsigned 64-bit conversion, with the low bit kept for correct rounding.
   */
  CHECK(15, (double)opaque(UINT64_C(0x8000000000000000)) == 9223372036854775808.0);
  CHECK(16, (double)opaque(UINT64_C(0x8000000000000401)) == 9223372036854777856.0);
  CHECK(17, (double)opaque(5) == 5.0);
  CHECK(18, (float)opaque(UINT64_C(0xffffffffffffffff)) == 18446744073709551616.0f);
  return 0;
}
