typedef unsigned int u32;
extern u32 register_run_callback(u32, u32, u32);
extern u32 register_run_stack_callback(u32 *, u32 *, u32);
#ifdef NYC_REFERENCE
#define RUN_FUNCTION reference_register_run
#define STACK_FUNCTION reference_stack_run
#else
#define RUN_FUNCTION nyc_register_run
#define STACK_FUNCTION nyc_stack_run
#endif

u32 RUN_FUNCTION(u32 *a, u32 *b, u32 seed) {
  u32 x0 = a[0] + seed, x1 = a[1] ^ seed, x2 = a[2] - seed;
  u32 x3 = a[3] * 17u, x4 = a[4] + x0, x5 = a[5] ^ x1;
  u32 x6 = a[6] - x2, x7 = a[7] + x3, x8 = a[8] * 33u;
  u32 x9 = a[9] ^ x4, x10 = a[10] + x5, x11 = a[11] - x6;
  b[0] = (x0 + x1) ^ (x2 * x3);
  ((unsigned char *)b)[5] = (unsigned char)(x4 + x5);
  u32 after = a[0] + a[1] + a[2];
  u32 total = (x0 ^ x6) + (x1 ^ x7) + (x2 ^ x8) + (x3 ^ x9);
  total ^= (x4 + x10) * (x5 + x11);
  if (seed & 1u)
    total += register_run_callback(x8, x9, x10);
  else
    total ^= register_run_callback(x11, x0, x6);
  for (u32 i = 0; i < (seed & 7u); ++i) {
    u32 old = a[i];
    b[i] = (old ^ total) + i;
    total = (total * 33u) ^ a[i] ^ x7;
  }
  b[8] = x0 + x2 + x4 + x6 + x8 + x10;
  b[9] = x1 ^ x3 ^ x5 ^ x7 ^ x9 ^ x11;
  return total + after + b[8] + b[9];
}
#undef RUN_FUNCTION

u32 STACK_FUNCTION(u32 seed) {
  u32 values[16];
  u32 local = seed * 37u;
  for (u32 i = 0; i < 16; ++i) values[i] = (seed ^ i) * 7919u;
  u32 result = register_run_stack_callback(&local, values, seed);
  for (u32 i = 0; i < 16; ++i) result = (result * 33u) ^ values[i];
  return result + local;
}
#undef STACK_FUNCTION
