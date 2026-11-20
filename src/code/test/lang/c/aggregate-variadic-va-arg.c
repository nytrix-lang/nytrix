/*
 * C frontend test fixture: aggregate variadic va arg.
 */
#include "aggregate-variadic-va-arg.h"

typedef __builtin_va_list va_list;

__attribute__((noinline)) static int read_mixed(va_list arguments) {
  struct va_mixed direct = __builtin_va_arg(arguments, struct va_mixed);
  struct va_reverse reverse = __builtin_va_arg(arguments, struct va_reverse);
  return direct.integer == 43 && direct.real == 4.5 && reverse.real == 5.25 &&
         reverse.integer == 53;
}

int va_registers(int marker, ...) {
  va_list arguments;
  __builtin_va_start(arguments, marker);
  unsigned uid = __builtin_va_arg(arguments, struct va_uid).value;
  double real = __builtin_va_arg(arguments, struct va_real).value;
  struct va_pair pair = __builtin_va_arg(arguments, struct va_pair);
  struct va_reals reals = __builtin_va_arg(arguments, struct va_reals);
  int mixed = read_mixed(arguments);
  unsigned long tail = __builtin_va_arg(arguments, unsigned long);
  __builtin_va_end(arguments);
  return marker == 7 && uid == 42 && real == 1.25 && pair.first == 11 &&
         pair.second == 13 && reals.first == 2.25 && reals.second == 3.5 &&
         mixed && tail == 59;
}

int va_gp_overflow(int marker, ...) {
  va_list arguments;
  __builtin_va_start(arguments, marker);
  for (int i = 0; i < 4; ++i)
    __builtin_va_arg(arguments, unsigned long);
  struct va_pair pair = __builtin_va_arg(arguments, struct va_pair);
  unsigned long last_gp = __builtin_va_arg(arguments, unsigned long);
  struct va_mixed mixed = __builtin_va_arg(arguments, struct va_mixed);
  double first_sse = __builtin_va_arg(arguments, double);
  __builtin_va_end(arguments);
  return pair.first == 11 && pair.second == 13 && last_gp == 17 &&
         mixed.integer == 43 && mixed.real == 4.5 && first_sse == 6.25;
}

int va_sse_overflow(int marker, ...) {
  va_list arguments;
  __builtin_va_start(arguments, marker);
  for (int i = 0; i < 7; ++i)
    __builtin_va_arg(arguments, double);
  struct va_reals reals = __builtin_va_arg(arguments, struct va_reals);
  double last_sse = __builtin_va_arg(arguments, double);
  struct va_reverse mixed = __builtin_va_arg(arguments, struct va_reverse);
  unsigned long first_gp = __builtin_va_arg(arguments, unsigned long);
  __builtin_va_end(arguments);
  return reals.first == 2.25 && reals.second == 3.5 && last_sse == 6.25 &&
         mixed.real == 5.25 && mixed.integer == 53 && first_gp == 59;
}

int va_memory(int marker, ...) {
  va_list arguments;
  __builtin_va_start(arguments, marker);
  for (int i = 0; i < 5; ++i)
    __builtin_va_arg(arguments, unsigned long);
  unsigned long before = __builtin_va_arg(arguments, unsigned long);
  struct va_aligned aligned = __builtin_va_arg(arguments, struct va_aligned);
  struct va_wide wide = __builtin_va_arg(arguments, struct va_wide);
  unsigned long after = __builtin_va_arg(arguments, unsigned long);
  __builtin_va_end(arguments);
  return before == 61 && aligned.a == 67 && aligned.b == 71 &&
         aligned.c == 73 && wide.a == 79 && wide.b == 83 && wide.c == 89 &&
         after == 97;
}
