/*
 * Unsupported compound templates must not turn into invented instructions.
 */
unsigned long transform(unsigned long *memory, unsigned long expected,
                        unsigned long replacement) {
#ifdef NYC_ASM_EXCHANGE
  __asm__ volatile("lock cmpxchgq %2, %1\n\tbswap %0"
                   : "+a"(expected), "+m"(*memory)
                   : "r"(replacement)
                   : "memory", "cc");
#else
  __asm__ volatile("movq %%gs:%1, %0\n\tbswap %0"
                   : "=r"(expected)
                   : "m"(*memory)
                   : "memory");
#endif
  return expected;
}
int main(void) {
  unsigned long memory = 7;
  return transform(&memory, 7, 11) != 7;
}
