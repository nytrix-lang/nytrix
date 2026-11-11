/*
 * Fixture: check unsigned constant casts, overflow, and arithmetic results.
 */
_Static_assert((unsigned short)~0U == 65535U,
               "constant integer casts truncate to their destination width");
int nyc_u64_add(unsigned long long a, unsigned long long b,
                unsigned long long *out) {
  return __builtin_add_overflow(a, b, out);
}
int nyc_u64_sub(unsigned long long a, unsigned long long b,
                unsigned long long *out) {
  return __builtin_sub_overflow(a, b, out);
}
int nyc_u64_mul(unsigned long long a, unsigned long long b,
                unsigned long long *out) {
  return __builtin_mul_overflow(a, b, out);
}
int nyc_i32_mul(int a, unsigned int b, int *out) {
  return __builtin_mul_overflow(a, b, out);
}
int nyc_u32_add(unsigned int a, unsigned int b, unsigned int *out) {
  return __builtin_add_overflow(a, b, out);
}
int nyc_u32_sub(unsigned int a, unsigned int b, unsigned int *out) {
  return __builtin_sub_overflow(a, b, out);
}
int nyc_u32_mul(unsigned int a, unsigned int b, unsigned int *out) {
  return __builtin_mul_overflow(a, b, out);
}
int main(void) {
  unsigned long long result = 0;
  unsigned int unsigned_result = 0;
  int narrow_result = 0;
  if (nyc_u64_add(~0ULL, 1, &result) != 1 || result != 0)
    return 1;
  if (nyc_u64_add(40, 2, &result) != 0 || result != 42)
    return 2;
  if (nyc_u64_sub(0, 1, &result) != 1 || result != ~0ULL)
    return 3;
  if (nyc_u64_sub(42, 2, &result) != 0 || result != 40)
    return 4;
  if (nyc_u64_mul(~0ULL, 2, &result) != 1 || result != 0xFFFFFFFFFFFFFFFEULL)
    return 5;
  if (nyc_u64_mul(0xFFFFFFFFULL, 0xFFFFFFFFULL, &result) != 0 ||
      result != 0xFFFFFFFE00000001ULL)
    return 6;
  if (nyc_u64_mul(0x100000000ULL, 0x100000000ULL, &result) != 1 || result != 0)
    return 7;
  if (nyc_i32_mul(1000, 1000U, &narrow_result) != 0 || narrow_result != 1000000)
    return 8;
  if (nyc_i32_mul(50000, 50000U, &narrow_result) != 1 ||
      narrow_result != -1794967296)
    return 9;
  if (nyc_u32_add(~0U, 1U, &unsigned_result) != 1 || unsigned_result != 0)
    return 10;
  if (nyc_u32_add(40U, 2U, &unsigned_result) != 0 || unsigned_result != 42U)
    return 11;
  if (nyc_u32_sub(0U, 1U, &unsigned_result) != 1 ||
      unsigned_result != 0xFFFFFFFFU)
    return 12;
  if (nyc_u32_mul(~0U, 2U, &unsigned_result) != 1 ||
      unsigned_result != 0xFFFFFFFEU)
    return 13;
  if (nyc_u32_mul(65535U, 65535U, &unsigned_result) != 0 ||
      unsigned_result != 0xFFFE0001U)
    return 14;
  if (~0U != 0xFFFFFFFFU || ~0 != -1)
    return 15;
  return 0;
}
