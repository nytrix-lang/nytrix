extern unsigned long asm_external_memory_value;
unsigned long read_asm_external_memory(void) {
  unsigned long value;
  __asm__ volatile("movq %[memory], %[result]"
                   : [result] "=r" (value)
                   : [memory] "m" (asm_external_memory_value));
  return value;
}
