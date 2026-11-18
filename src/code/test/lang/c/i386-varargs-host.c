/*
 * Host-side C counterpart for the i386 varargs test.
 */
struct pair {
  int first, second;
};
extern long long i386_va_scalars(int, ...);
extern int i386_va_fixed(struct pair, double, int, ...);
extern struct pair i386_va_sret(int, ...);
extern int i386_call_host(void);
extern double i386_wide_to_double(long long);

int host_va_reader(int first, ...) {
  __builtin_va_list list;
  __builtin_va_start(list, first);
  int integer = __builtin_va_arg(list, int);
  long long wide = __builtin_va_arg(list, long long);
  double real = __builtin_va_arg(list, double);
  __builtin_va_end(list);
  return first + integer + (int)wide + (int)real;
}

void _start(void) {
  int value = 29, status = 0;
  long long result = i386_va_scalars(3, -7, -0x100000003LL, 17.75, &value);
  if (result != -0x100000003LL + 21)
    status = 1;
  struct pair fixed = {5, 7}, extra = {11, 13};
  if (i386_va_fixed(fixed, 17.75, 19, extra, 23) != 95)
    status = 2;
  struct pair returned = i386_va_sret(31, 37);
  if (returned.first != 31 || returned.second != 37)
    status = 3;
  if (i386_call_host() != 28)
    status = 4;
  if (i386_wide_to_double(-0x100000003LL) != -4294967299.0)
    status = 5;
  unsigned short original, restored;
  __asm__ volatile("fnstcw %0" : "=m"(original));
  unsigned short upward = (original & ~0xc00u) | 0x800u;
  __asm__ volatile("fldcw %0" : : "m"(upward));
  result = i386_va_scalars(3, -7, -0x100000003LL, -17.75, &value);
  __asm__ volatile("fnstcw %0" : "=m"(restored));
  __asm__ volatile("fldcw %0" : : "m"(original));
  if (result != -0x100000003LL - 13)
    status = 6;
  if (restored != upward)
    status = 7;
  __asm__ volatile("int $0x80" : : "a"(1), "b"(status) : "memory");
  __builtin_unreachable();
}
