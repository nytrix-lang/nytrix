/* C frontend test fixture: aggregate variadic va_arg declarations. */
#ifndef NYC_TEST_AGGREGATE_VA_ARG_H
#define NYC_TEST_AGGREGATE_VA_ARG_H

struct va_uid {
  unsigned int value;
};
struct va_real {
  double value;
};
struct va_pair {
  unsigned long first, second;
};
struct va_reals {
  double first, second;
};
struct va_mixed {
  unsigned long integer;
  double real;
};
struct va_reverse {
  double real;
  unsigned long integer;
};
struct va_wide {
  unsigned long a, b, c;
};
struct va_aligned {
  unsigned long a, b, c;
} __attribute__((aligned(16)));

int va_registers(int marker, ...);
int va_gp_overflow(int marker, ...);
int va_sse_overflow(int marker, ...);
int va_memory(int marker, ...);

#endif
