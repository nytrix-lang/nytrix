/*
 * C parser fixture for va_start offsets after fixed aggregate parameters.
 */
typedef __builtin_va_list va_list;

struct nyc_va_pair {
  unsigned long first;
  unsigned long second;
};

struct nyc_va_mixed {
  double real;
  unsigned long integer;
};

unsigned long nyc_va_after_stack_aggregate(unsigned long a, unsigned long b,
                                           unsigned long c, unsigned long d,
                                           unsigned long e,
                                           struct nyc_va_pair fixed, ...) {
  va_list arguments;
  __builtin_va_start(arguments, fixed);
  unsigned long in_register = __builtin_va_arg(arguments, unsigned long);
  unsigned long on_stack = __builtin_va_arg(arguments, unsigned long);
  __builtin_va_end(arguments);
  return a + b + c + d + e + fixed.first + fixed.second + in_register +
         on_stack;
}

int nyc_va_after_mixed_aggregate(struct nyc_va_mixed fixed, ...) {
  va_list arguments;
  __builtin_va_start(arguments, fixed);
  double real = __builtin_va_arg(arguments, double);
  unsigned long integer = __builtin_va_arg(arguments, unsigned long);
  __builtin_va_end(arguments);
  return fixed.real == 1.25 && fixed.integer == 3 && real == 2.5 && integer == 7
             ? 0
             : 1;
}
