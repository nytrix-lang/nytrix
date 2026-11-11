/*
 * Fixture: compile i386 vDSO arithmetic helpers with explicit integer widths.
 */
typedef unsigned int u32;
typedef unsigned long long u64;

static u64 nyc_i386_division(u64 dividend, u64 divisor) {
  return dividend / divisor + dividend % divisor;
}

static long nyc_i386_syscall(u32 number, u32 argument) {
  register long fixed_argument __asm__("ebx") = (long)argument;
  __asm__ volatile("ds;ds;ds;int $0x80; call __kernel_vsyscall"
                   : "+a"(number)
                   : "r"(fixed_argument)
                   : "memory");
  return (long)number;
}

int main(void) {
  return (int)(nyc_i386_division(17, 5) + (u64)nyc_i386_syscall(1, 2));
}
