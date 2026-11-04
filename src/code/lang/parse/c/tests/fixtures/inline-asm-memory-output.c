unsigned long nyc_inline_asm_memory_output(void) {
  unsigned long flags;
  asm volatile("pushf; pop %0" : "=rm"(flags) : : "memory");
  return flags & 2;
}
