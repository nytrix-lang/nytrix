/*
 * Exercise memory arithmetic across the 32-bit carry and borrow boundary.
 */
typedef unsigned int u32;
typedef unsigned long long u64;
#define OP(name, type, suffix, operation)                     \
  type name##suffix(type a, type b) { return a operation b; }
#define OPS(type, suffix)  \
  OP(add, type, suffix, +) \
  OP(sub, type, suffix, -) \
  OP(and, type, suffix, &) \
  OP(or, type, suffix, |)  \
  OP(xor, type, suffix, ^)
OPS(u32, 32)
OPS(u64, 64)
OP(mul, u32, 32, *)
int signed_less(int a, int b) { return a < b; }
int unsigned_less(u32 a, u32 b) { return a < b; }
#define PROMOTED(bits, type)                                    \
  int promoted##bits(u64 a, u64 b) {                            \
    type x = (type)a, y = (type)b;                              \
    return (x < y) | ((x <= y) << 1) | ((x > y) << 2) |         \
           ((x >= y) << 3) | ((x == y) << 4) | ((x != y) << 5); \
  }
PROMOTED(8, signed char)
PROMOTED(16, short)
PROMOTED(32, int)
int mixed_width_less(u64 a, u64 b) { return (long long)(int)a < (u32)b; }
int shifted_less(u64 a, u64 b) {
  return ((long long)a >> 32) < ((long long)b >> 32);
}
int wide_less(u64 a, u64 b) { return (long long)a < (long long)b; }
long long signed_count_shift(u64 a, u32 count) {
  return (long long)a >> (count & 63);
}
long long mixed_divide(u64 a, u32 b) { return (long long)a / b; }
long long signed_divide(u64 a, u64 b) { return (long long)a / (long long)b; }
long long signed_remainder(u64 a, u64 b) { return (long long)a % (long long)b; }
u64 unsigned_divide(u64 a, u64 b) { return a / b; }
u64 unsigned_remainder(u64 a, u64 b) { return a % b; }
u64 left_high(u32 a) { return (u64)a << 32; }
long long signed_shift17(u64 a) { return (long long)a >> 17; }
long long signed_shift63(u64 a) { return (long long)a >> 63; }
u64 left_shift63(u32 a) { return (u64)a << 63; }
extern void mutate_wide(u64 *);
int cycle_bounds(u32 seed, u32 count) {
  int a = (int)seed, b = -19;
  for (u32 i = 0; i < (count & 31u); ++i) {
    int saved = a;
    a = b;
    b = saved;
  }
  return (a < 0) | ((a < b) << 1) | ((a == b) << 2) | ((b < 0) << 3);
}
u64 wide_loop(u64 seed, u32 count) {
  u64 accumulator = seed;
  u32 low = (u32)seed;
  for (u32 i = 0; i < (count & 31u); ++i) {
    accumulator += 0x100000001ULL;
    low = (low * 33u) ^ i;
  }
  return accumulator ^ low;
}
u64 wide_local(u64 seed) {
  u64 local = seed;
  mutate_wide(&local);
  return local;
}
u64 pick_wide(u64 condition, u64 yes, u64 no) { return condition ? yes : no; }
u32 pick_narrow(u64 condition, u32 yes, u32 no) { return condition ? yes : no; }
u32 branch_mix(u32 a, u32 b) {
  volatile u32 result = 0;
  if (a < b) result += 1;
  if (a <= b) result += 2;
  if (a > b) result += 4;
  if (a >= b) result += 8;
  if (a == b) result += 16;
  if (a != b) result += 32;
  if ((int)a < (int)b) result += 64;
  if ((int)a <= (int)b) result += 128;
  if ((int)a > (int)b) result += 256;
  if ((int)a >= (int)b) result += 512;
  return result;
}
