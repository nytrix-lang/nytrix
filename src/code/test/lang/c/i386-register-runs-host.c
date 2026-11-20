/*
 * Host-side C counterpart for the i386 register runs test.
 */
#define NYC_REFERENCE
#include "i386-register-runs.c"
extern u32 nyc_register_run(u32 *, u32 *, u32);
extern u32 nyc_stack_run(u32);
extern u32 nyc_inlined_asm_output(u32, u32, u32);

u32 register_run_callback(u32 a, u32 b, u32 c) {
  return (a * 65537u) ^ (b + 0x80000001u) ^ (c * 19u);
}

u32 register_run_four(u32 a, u32 b, u32 c, u32 d) {
  return (a * 31u + b * 101u) ^ (c + d * 257u);
}

u32 (*register_run_indirect)(u32, u32, u32, u32) = register_run_four;

u32 register_run_stack_callback(u32 *local, u32 *values, u32 seed) {
  *local ^= values[3] + seed;
  values[7] += *local;
  return values[7] ^ values[15];
}

static int check_register_runs(void) {
  for (u32 seed = 0; seed < 40; ++seed) {
    if (nyc_stack_run(seed) != reference_stack_run(seed))
      return 3;
    if (nyc_inlined_asm_output(seed, seed * 7919u, ~seed) !=
        reference_inlined_asm_output(seed, seed * 7919u, ~seed))
      return 4;
    for (u32 offset = 0; offset < 6; ++offset) {
      u32 actual[24], expected[24];
      for (u32 i = 0; i < 24; ++i)
        actual[i] = expected[i] = (seed + i * 7919u) * 0x9e3779b9u;
      u32 want = reference_register_run(expected, expected + offset, seed);
      u32 got = nyc_register_run(actual, actual + offset, seed);
      if (got != want)
        return 1;
      for (u32 i = 0; i < 24; ++i)
        if (actual[i] != expected[i])
          return 2;
    }
  }
  return 0;
}

#ifdef NYC_PROBE_HOSTED
int main(void) { return check_register_runs(); }
#else
__attribute__((force_align_arg_pointer)) void _start(void) {
  int status = check_register_runs();
  __asm__ volatile("int $0x80" : : "a"(1), "b"(status) : "memory");
  __builtin_unreachable();
}
#endif
