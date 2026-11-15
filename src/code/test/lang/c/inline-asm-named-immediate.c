/*
 * Inline assembly fixture for named immediate operands and modifiers.
 */
static void named_immediate(void) {
  __asm__ volatile("testq $%c[value], %%rax" : : [value] "i"(12) : "cc");
}
static unsigned long named_register_output(void) {
  unsigned long value;
  __asm__ volatile("movq $%c[input], %[value]"
                   : [value] "=r"(value)
                   : [input] "i"(12));
  return value;
}
static unsigned long named_local_constant(void) {
  unsigned long flags = (9UL << 8) | (1UL << 0) | (1UL << 4);
  unsigned long value;
  __asm__ volatile("movq $%c[flags], %[value]"
                   : [value] "=r"(value)
                   : [flags] "i"(flags));
  return value;
}
static unsigned long named_absolute_input(void) {
  unsigned long address;
  __asm__ volatile("leaq %a[offset](%%rip), %[address]"
                   : [address] "=r"(address)
                   : [offset] "i"(0));
  return address;
}
int main(void) {
  named_immediate();
  return named_register_output() != 12 ||
         named_local_constant() != ((9UL << 8) | (1UL << 0) | (1UL << 4)) ||
         named_absolute_input() == 0;
}
