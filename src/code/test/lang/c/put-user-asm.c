/*
 * Fixture: parse register-bound x86 put-user inline assembly.
 */
register unsigned long current_stack_pointer __asm__("rsp");
int put_user_asm_probe(unsigned long *address, unsigned long value) {
  int result;
  __asm__ volatile("call __put_user_%c[size]"
                   : "=c"(result), "+r"(current_stack_pointer)
                   : "0"(address), "r"(value), [size] "i"(sizeof(*address))
                   : "ebx");
  return result;
}
