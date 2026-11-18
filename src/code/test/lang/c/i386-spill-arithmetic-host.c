/*
 * Host-side C counterpart for the i386 spill arithmetic test.
 */
typedef unsigned int u32;
typedef unsigned long long u64;
#define DECL(name, type, suffix) extern type name##suffix(type, type);
#define DECLS(type, suffix)                                               \
  DECL(add, type, suffix) DECL(sub, type, suffix) DECL(and, type, suffix) \
  DECL(or, type, suffix) DECL(xor, type, suffix)
DECLS(u32, 32)
DECLS(u64, 64)
DECL(mul, u32, 32)
extern int signed_less(int, int);
extern int unsigned_less(u32, u32);
extern u32 branch_mix(u32, u32);
extern int promoted8(u64, u64), promoted16(u64, u64), promoted32(u64, u64);
extern int mixed_width_less(u64, u64), shifted_less(u64, u64), wide_less(u64, u64);
extern long long signed_count_shift(u64, u32), mixed_divide(u64, u32);
extern u64 left_high(u32);
extern long long signed_shift17(u64), signed_shift63(u64);
extern u64 left_shift63(u32);
extern u64 wide_loop(u64, u32), wide_local(u64);
extern int cycle_bounds(u32, u32);
extern u64 pick_wide(u64, u64, u64);
extern u32 pick_narrow(u64, u32, u32);
void mutate_wide(u64 *value) { *value ^= 0x8000000100000003ULL; }
extern long long signed_divide(u64, u64), signed_remainder(u64, u64);
extern u64 unsigned_divide(u64, u64), unsigned_remainder(u64, u64);

/*
 * Independent bit-at-a-time oracle avoids a host compiler-runtime dependency
 * in this freestanding 32-bit executable. Carry is part of the remainder.
 */
static u64 divide_reference(u64 a, u64 b, u64 *rest) {
  u64 q = 0, r = 0;
  for (unsigned bit = 0; bit < 64; ++bit) {
    unsigned carry = (unsigned)(r >> 63);
    r = (r << 1) | (a >> 63);
    a <<= 1;
    q <<= 1;
    if (carry || r >= b) { r -= b; q |= 1; }
  }
  *rest = r;
  return q;
}

static int check(void) {
  static const u64 values[] = {
      0, 1, 127, 128, 255, 256, 32767, 32768, 65535, 65536,
      0x7fffffffULL, 0x80000000ULL, 0xffffffffULL,
      0x100000000ULL, 0x100000001ULL, 0x7fffffffffffffffULL,
      0x8000000000000000ULL, 0xffffffffffffffffULL};
  for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
    for (unsigned j = 0; j < sizeof(values) / sizeof(values[0]); ++j) {
      u64 a = values[i], b = values[j];
      u32 x = (u32)a, y = (u32)b;
      int first = (y & 1) ? -19 : (int)x;
      int second = (y & 1) ? (int)x : -19;
      int cycle_expected = (first < 0) | ((first < second) << 1) |
                           ((first == second) << 2) | ((second < 0) << 3);
      if (cycle_bounds(x, y) != cycle_expected) return 12;
#define CHECK(name, suffix, a, b, operation)                   \
      if (name##suffix(a, b) != ((a) operation (b))) return 1;
#define CHECKS(suffix, a, b)                                  \
      CHECK(add, suffix, a, b, +) CHECK(sub, suffix, a, b, -) \
      CHECK(and, suffix, a, b, &) CHECK(or, suffix, a, b, |)  \
      CHECK(xor, suffix, a, b, ^)
      CHECKS(32, x, y)
      CHECKS(64, a, b)
      CHECK(mul, 32, x, y, *)
      if (signed_less((int)x, (int)y) != ((int)x < (int)y) ||
          unsigned_less(x, y) != (x < y))
        return 2;
      u32 expected = (x < y) + 2u * (x <= y) + 4u * (x > y) +
          8u * (x >= y) + 16u * (x == y) + 32u * (x != y) +
          64u * ((int)x < (int)y) + 128u * ((int)x <= (int)y) +
          256u * ((int)x > (int)y) + 512u * ((int)x >= (int)y);
      if (branch_mix(x, y) != expected)
        return 3;
#define CHECK_PROMOTED(bits, type) {                                    \
        type l = (type)a, r = (type)b;                                  \
        int want = (l < r) | ((l <= r) << 1) | ((l > r) << 2) |         \
                   ((l >= r) << 3) | ((l == r) << 4) | ((l != r) << 5); \
        if (promoted##bits(a, b) != want) return 4;                     \
      }
      CHECK_PROMOTED(8, signed char)
      CHECK_PROMOTED(16, short)
      CHECK_PROMOTED(32, int)
      if (mixed_width_less(a, b) != ((long long)(int)a < (u32)b) ||
          shifted_less(a, b) != (((long long)a >> 32) < ((long long)b >> 32)) ||
          wide_less(a, b) != ((long long)a < (long long)b))
        return 5;
      if (signed_count_shift(a, y) != ((long long)a >> (y & 63)) ||
          left_high(x) != ((u64)x << 32) ||
          signed_shift17(a) != ((long long)a >> 17) ||
          signed_shift63(a) != ((long long)a >> 63) ||
          left_shift63(x) != ((u64)x << 63))
        return 6;
      u64 accumulated = a;
      u32 low = x;
      for (u32 n = 0; n < (y & 31u); ++n) {
        accumulated += 0x100000001ULL;
        low = (low * 33u) ^ n;
      }
      if (wide_loop(a, y) != (accumulated ^ low) ||
          wide_local(a) != (a ^ 0x8000000100000003ULL))
        return 10;
      if (pick_wide(a, b, ~b) != (a ? b : ~b) ||
          pick_narrow(a, y, ~y) != (a ? y : ~y))
        return 11;
      if (b) {
        u64 rest;
        u64 q = divide_reference(a, b, &rest);
        if (unsigned_divide(a, b) != q || unsigned_remainder(a, b) != rest)
          return 8;
        if (!(a == 0x8000000000000000ULL && b == 0xffffffffffffffffULL)) {
          u64 magnitude_a = (long long)a < 0 ? 0 - a : a;
          u64 magnitude_b = (long long)b < 0 ? 0 - b : b;
          q = divide_reference(magnitude_a, magnitude_b, &rest);
          if (((a ^ b) >> 63)) q = 0 - q;
          if (a >> 63) rest = 0 - rest;
          if ((u64)signed_divide(a, b) != q ||
              (u64)signed_remainder(a, b) != rest)
            return 9;
        }
      }
    }
  }
  if (mixed_divide(0xfffffffffffffff9ULL, 2) != -3 ||
      mixed_divide(0x8000000000000000ULL, 0xffffffffu) != -2147483648LL)
    return 7;
  return 0;
}

#ifdef NYC_PROBE_HOSTED
int main(void) { return check(); }
#else
__attribute__((force_align_arg_pointer)) void _start(void) {
  int status = check();
  __asm__ volatile("int $0x80" : : "a"(1), "b"(status) : "memory");
  __builtin_unreachable();
}
#endif
