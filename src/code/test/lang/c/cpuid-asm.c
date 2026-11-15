/*
 * Fixture: read x86 CPUID outputs with explicit register constraints.
 */
typedef unsigned int u32;
void nyc_cpuid_probe(u32 *eax, u32 *ebx, u32 *ecx, u32 *edx) {
  __asm__ volatile("cpuid"
                   : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                   : "0"(*eax), "2"(*ecx)
                   : "memory");
}

void nyc_cpuid_no_clobbers(u32 *eax, u32 *ebx, u32 *ecx, u32 *edx) {
  __asm__ volatile("cpuid"
                   : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                   : "0"(*eax), "2"(*ecx));
}

void nyc_cpuid_reordered(u32 *eax, u32 *ebx, u32 *ecx, u32 *edx) {
  __asm__ volatile("cpuid"
                   : "=d"(*edx), "=c"(*ecx), "=b"(*ebx), "=a"(*eax)
                   : "3"(*eax), "1"(*ecx));
}

void nyc_cpuid_readwrite(u32 *eax, u32 *ebx, u32 *ecx, u32 *edx) {
  u32 a = *eax, c = *ecx;
  __asm__ volatile("cpuid" : "+a"(a), "+c"(c), "=b"(*ebx), "=d"(*edx));
  *eax = a;
  *ecx = c;
}

u32 nyc_cpuid_partial(void) {
  u32 level = 0;
  __asm__("cpuid" : "+a"(level) : : "edx", "ecx", "ebx");
  return level;
}

unsigned long nyc_cpuid_live_value(unsigned long value) {
  u32 a = 0, b, c = 0, d;
  __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
  return value ^ a ^ b ^ c ^ d;
}
