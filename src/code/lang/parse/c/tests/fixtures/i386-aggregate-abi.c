/*
 * Fixture: exercise aggregate argument and return rules for the i386 ABI.
 */
#include "i386-aggregate-abi.h"

int nyc_i386_registers(int a, int b, int c, int d) {
  return a + 2 * b + 3 * c + 4 * d;
}
int nyc_i386_callback(nyc_i386_callback_fn callback) {
  return callback(3, -7, 11, 19);
}
long long nyc_i386_wide(int prefix, long long value, int suffix) {
  return prefix + value * 3 + suffix;
}
int nyc_i386_real(struct nyc_i386_real value, int a, int b, int c) {
  return value.value == 2.5 ? a + 2 * b + 3 * c : -1;
}
int nyc_i386_cdecl(int a, int b, int c, int d) {
  return a + 2 * b + 3 * c + 4 * d;
}

struct nyc_i386_tiny nyc_i386_tiny(int value) {
  struct nyc_i386_tiny result = {value, value + 1, value + 2};
  return result;
}

struct nyc_i386_mixed nyc_i386_mix(int prefix, struct nyc_i386_tiny tiny,
                                   double scale, struct nyc_i386_mixed input,
                                   int suffix) {
  input.value += prefix + tiny.a + suffix;
  input.real *= scale;
  input.tag += tiny.c;
  return input;
}

struct nyc_i386_large nyc_i386_large(struct nyc_i386_large input, int delta) {
  for (int i = 0; i < 7; ++i)
    input.values[i] += delta + i;
  return input;
}

int nyc_i386_verify(void) {
  struct nyc_i386_tiny tiny = {3, 4, 5};
  struct nyc_i386_tiny changed = host_i386_tiny(tiny, 7);
  if (changed.a != 10 || changed.b != 11 || changed.c != 12 || tiny.a != 3)
    return 1;
  struct nyc_i386_mixed input = {17, 1.5, 9};
  struct nyc_i386_mixed result = host_i386_mix(2, tiny, 2.0, input, 6);
  if (result.value != 28 || result.real != 3.0 || result.tag != 14)
    return 2;
  if (input.value != 17 || input.real != 1.5 || input.tag != 9)
    return 3;
  if (host_i386_registers(3, -7, 11, 19) != 98 ||
      host_i386_callback(nyc_i386_registers) != 98 ||
      host_i386_cdecl(3, -7, 11, 19) != 98)
    return 4;
  if (host_i386_wide(5, -4294967298LL, 9) != -12884901880LL)
    return 5;
  struct nyc_i386_real real = {2.5};
  if (host_i386_real(real, 3, 5, 7) != 34)
    return 6;
  if (!host_i386_sizes(71, sizeof(input), __alignof__(struct nyc_i386_mixed),
                       __builtin_offsetof(struct nyc_i386_mixed, real),
                       0x12345678U))
    return 8;
  struct nyc_i386_tiny *initialized[2] = {&tiny, &changed};
  struct nyc_i386_tiny *local[2];
  local[0] = initialized[0];
  local[1] = initialized[1];
  local[1]->b = 47;
  if (tiny.a != 3 || changed.b != 47 || changed.a != 10 || changed.c != 12 ||
      sizeof(local) != 2 * sizeof(void *) ||
      (char *)&local[1] - (char *)&local[0] != sizeof(void *))
    return 7;
  return 0;
}
