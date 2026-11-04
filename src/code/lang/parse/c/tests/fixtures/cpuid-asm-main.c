typedef unsigned int u32;
void nyc_cpuid_probe(u32 *, u32 *, u32 *, u32 *);
static void host_cpuid(u32 *eax, u32 *ebx, u32 *ecx, u32 *edx) {
  __asm__ volatile("cpuid"
                   : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                   : "0"(*eax), "2"(*ecx)
                   : "memory");
}
int main(void) {
  u32 actual[4] = {0, 0, 0, 0};
  u32 expected[4] = {0, 0, 0, 0};
  nyc_cpuid_probe(&actual[0], &actual[1], &actual[2], &actual[3]);
  host_cpuid(&expected[0], &expected[1], &expected[2], &expected[3]);
  return actual[0] != expected[0] || actual[1] != expected[1] ||
         actual[2] != expected[2] || actual[3] != expected[3];
}
