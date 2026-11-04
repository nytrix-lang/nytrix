register unsigned long current_stack_pointer __asm__("rsp");
unsigned long get_user_asm_probe(unsigned long *address) {
  long result;
  register unsigned long value __asm__("%rdx");
  __asm__ volatile("call __get_user_%c[size]"
                   : "=a"(result), "=r"(value), "+r"(current_stack_pointer)
                   : "0"(address), [size] "i"(sizeof(*address)));
  value = (__typeof__(*address))value;
  return (unsigned long)result + value;
}

unsigned long get_user_asm_probe32(unsigned long *address) {
  int result;
  register unsigned long value __asm__("%rdx");
  __asm__ volatile("call __get_user_%c[size]"
                   : "=a"(result), "=r"(value), "+r"(current_stack_pointer)
                   : "0"(address), [size] "i"(sizeof(*address)));
  return (unsigned long)result + value;
}
