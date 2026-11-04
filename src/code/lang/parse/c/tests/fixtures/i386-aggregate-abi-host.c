#include "i386-aggregate-abi.h"

int host_i386_registers(int a, int b, int c, int d) {
  return a + 2 * b + 3 * c + 4 * d;
}
int host_i386_callback(nyc_i386_callback_fn callback) {
  return callback(3, -7, 11, 19);
}
long long host_i386_wide(int prefix, long long value, int suffix) {
  return prefix + value * 3 + suffix;
}
int host_i386_real(struct nyc_i386_real value, int a, int b, int c) {
  return value.value == 2.5 ? a + 2 * b + 3 * c : -1;
}
int host_i386_cdecl(int a, int b, int c, int d) {
  return a + 2 * b + 3 * c + 4 * d;
}

int host_i386_sizes(int marker, ...) {
  __builtin_va_list arguments;
  __builtin_va_start(arguments, marker);
  unsigned int size = __builtin_va_arg(arguments, unsigned int);
  unsigned int alignment = __builtin_va_arg(arguments, unsigned int);
  unsigned int offset = __builtin_va_arg(arguments, unsigned int);
  unsigned int sentinel = __builtin_va_arg(arguments, unsigned int);
  __builtin_va_end(arguments);
  return marker == 71 && size == sizeof(struct nyc_i386_mixed) &&
         alignment == __alignof__(struct nyc_i386_mixed) &&
         offset == __builtin_offsetof(struct nyc_i386_mixed, real) &&
         sentinel == 0x12345678U;
}

struct nyc_i386_tiny host_i386_tiny(struct nyc_i386_tiny input, int delta) {
  input.a += delta;
  input.b += delta;
  input.c += delta;
  return input;
}

struct nyc_i386_mixed host_i386_mix(int prefix, struct nyc_i386_tiny tiny,
                                    double scale, struct nyc_i386_mixed input,
                                    int suffix) {
  input.value += prefix + tiny.a + suffix;
  input.real *= scale;
  input.tag += tiny.c;
  return input;
}

static int verify(void) {
  /*
   * Adjacent bytes expose an over-wide copy of a three-byte return value.
   */
  struct {
    unsigned char before;
    struct nyc_i386_tiny value;
    unsigned char after;
  } guarded = {91, {0, 0, 0}, 93};
  guarded.value = nyc_i386_tiny(11);
  if (guarded.before != 91 || guarded.after != 93 || guarded.value.a != 11 ||
      guarded.value.b != 12 || guarded.value.c != 13)
    return 10;
  struct nyc_i386_mixed input = {7, 2.5, 4};
  struct nyc_i386_mixed result = nyc_i386_mix(3, guarded.value, 2.0, input, 5);
  if (result.value != 26 || result.real != 5.0 || result.tag != 17 ||
      input.value != 7 || input.real != 2.5 || input.tag != 4)
    return 11;
  struct nyc_i386_large large = {{1, 2, 3, 4, 5, 6, 7}};
  struct nyc_i386_large transformed = nyc_i386_large(large, 9);
  for (int i = 0; i < 7; ++i)
    if (transformed.values[i] != 10 + 2 * i || large.values[i] != i + 1)
      return 12;
  if (nyc_i386_registers(3, -7, 11, 19) != 98 ||
      nyc_i386_callback(host_i386_registers) != 98 ||
      nyc_i386_cdecl(3, -7, 11, 19) != 98)
    return 13;
  if (nyc_i386_wide(5, -4294967298LL, 9) != -12884901880LL)
    return 14;
  struct nyc_i386_real real = {2.5};
  if (nyc_i386_real(real, 3, 5, 7) != 34)
    return 15;
  return nyc_i386_verify();
}

__attribute__((force_align_arg_pointer)) void _start(void) {
  int status = verify();
  __asm__ volatile("int $0x80" : : "a"(1), "b"(status) : "memory");
  __builtin_unreachable();
}
