/*
 * Runtime harness for CPUID inline assembly output-register tests.
 */
typedef unsigned int u32;
void nyc_cpuid_probe(u32 *, u32 *, u32 *, u32 *);
void nyc_cpuid_no_clobbers(u32 *, u32 *, u32 *, u32 *);
void nyc_cpuid_reordered(u32 *, u32 *, u32 *, u32 *);
void nyc_cpuid_readwrite(u32 *, u32 *, u32 *, u32 *);
u32 nyc_cpuid_partial(void);
unsigned long nyc_cpuid_live_value(unsigned long);
static void host_cpuid(u32 *eax, u32 *ebx, u32 *ecx, u32 *edx) {
  __asm__ volatile("cpuid"
                   : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                   : "0"(*eax), "2"(*ecx)
                   : "memory");
}
int main(void) {
  void (*probes[])(u32 *, u32 *, u32 *,
                   u32 *) = {nyc_cpuid_probe, nyc_cpuid_no_clobbers,
                             nyc_cpuid_reordered, nyc_cpuid_readwrite};
  for (unsigned probe = 0; probe < sizeof(probes) / sizeof(probes[0]); ++probe)
    for (unsigned leaf = 0; leaf <= 7; leaf += 7) {
      u32 actual[4] = {leaf, 0, 0, 0};
      u32 expected[4] = {leaf, 0, 0, 0};
      probes[probe](&actual[0], &actual[1], &actual[2], &actual[3]);
      host_cpuid(&expected[0], &expected[1], &expected[2], &expected[3]);
      for (unsigned reg = 0; reg < 4; ++reg)
        if (actual[reg] != expected[reg])
          return 1 + (int)probe;
    }
  u32 expected[4] = {0, 0, 0, 0};
  host_cpuid(&expected[0], &expected[1], &expected[2], &expected[3]);
  if (nyc_cpuid_partial() != expected[0])
    return 5;
  unsigned long input = ~0UL - 123;
  if (nyc_cpuid_live_value(input) !=
      (input ^ expected[0] ^ expected[1] ^ expected[2] ^ expected[3]))
    return 6;
#if defined(__x86_64__)
  register unsigned long saved __asm__("rbx") = input;
  __asm__ volatile("" : "+r"(saved));
  nyc_cpuid_partial();
  __asm__ volatile("" : "+r"(saved));
  if (saved != input)
    return 7;
#endif
  return 0;
}

#ifdef NYC_I386_FREESTANDING
void _start(void) {
  int status = main();
  __asm__ volatile("int $0x80" : : "a"(1), "b"(status) : "memory");
  __builtin_unreachable();
}
#endif
