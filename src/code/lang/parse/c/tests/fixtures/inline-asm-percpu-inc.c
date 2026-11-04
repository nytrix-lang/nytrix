void nyc_inline_asm_percpu_inc(unsigned long *value) {
  asm volatile("incq %%gs:%[var]" : [var] "+m"(*value));
}
void nyc_inline_asm_percpu_dec(unsigned long *value) {
  asm volatile("decq %%gs:%[var]" : [var] "+m"(*value));
}
void nyc_inline_asm_percpu_add(unsigned long *value, unsigned long delta) {
  asm volatile("addq %[val], %%gs:%[var]" : [var] "+m"(*value) : [val] "r"(delta));
}
unsigned char nyc_inline_asm_percpu_dec_and_test(unsigned long *value) {
  unsigned char zero;
  asm volatile("decq %%gs:%[var]" : [var] "+m"(*value), "=@ccz"(zero) : : "memory");
  return zero;
}
unsigned long nyc_inline_asm_save_flags(void) {
  unsigned long flags;
  asm volatile("# __raw_save_flags\n\tpushf ; pop %0" : [flags] "=rm"(flags) : : "memory");
  return flags;
}
extern unsigned long current_task;
unsigned long nyc_inline_asm_percpu_symbol_read(void) {
  unsigned long value;
  asm volatile("movq %%gs:%[symbol], %[value]" : [value] "=r"(value) : [symbol] "i"(&current_task));
  return value;
}
