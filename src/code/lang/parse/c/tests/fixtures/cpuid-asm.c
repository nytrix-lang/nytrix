typedef unsigned int u32;
void nyc_cpuid_probe(u32 *eax, u32 *ebx, u32 *ecx, u32 *edx) {
  __asm__ volatile("cpuid"
                   : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                   : "0"(*eax), "2"(*ecx)
                   : "memory");
}
