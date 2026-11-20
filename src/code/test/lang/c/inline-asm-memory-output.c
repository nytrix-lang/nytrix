static __attribute__((always_inline)) inline unsigned long read_flags(void) {
  unsigned long flags;
  asm volatile("pushf; pop %0" : "=rm"(flags) : : "memory");
  return flags;
}

static __attribute__((always_inline)) inline unsigned long
overwrite_formal(unsigned long value) {
  asm volatile("movq $37,%0" : "=rm"(value) : : "memory");
  return value;
}

unsigned long nyc_inline_asm_memory_output(unsigned long first,
                                           unsigned long second,
                                           unsigned long third) {
  if (!(read_flags() & 2))
    return 0;
  return first + second + third + overwrite_formal(second);
}
