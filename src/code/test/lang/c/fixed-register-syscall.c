/*
 * Inline assembly fixture for syscall arguments bound to fixed registers.
 */
long nyc_fixed_register_syscall(void) {
  register unsigned long arg1 __asm__("rdi") = 0;
  register unsigned long arg2 __asm__("rsi") = 0;
  register unsigned long arg3 __asm__("rdx") = 0;
  long number = 39;
  long result;
  __asm__ volatile("syscall"
                   : "=a"(result)
                   : "0"(number), "D"(arg1), "S"(arg2), "d"(arg3)
                   : "rcx", "r11", "memory");
  return result;
}
int main(void) { return nyc_fixed_register_syscall() <= 0; }
