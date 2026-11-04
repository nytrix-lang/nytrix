unsigned long nyc_inline_asm_percpu_read(const unsigned long *value) {
  unsigned long result;
  asm volatile("movq %%gs:%[var], %[val]" : [val] "=r"(result) : [var] "m"(*value));
  return result;
}
