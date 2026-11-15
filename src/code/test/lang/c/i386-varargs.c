typedef __builtin_va_list va_list;
struct pair {
  int first, second;
};

static int next_copy(va_list list) { return __builtin_va_arg(list, int); }

long long i386_va_scalars(int first, ...) {
  va_list list, copy;
  __builtin_va_start(list, first);
  __builtin_va_copy(copy, list);
  int same = next_copy(list) + next_copy(list);
  int integer = __builtin_va_arg(list, int);
  long long wide = __builtin_va_arg(list, long long);
  double real = __builtin_va_arg(list, double);
  const int *pointer = __builtin_va_arg(list, const int *);
  int copied = __builtin_va_arg(copy, int);
  __builtin_va_end(copy);
  __builtin_va_end(list);
  return first + same + integer + wide + (long long)real + *pointer + copied;
}

int i386_va_fixed(struct pair pair, double real, int last, ...) {
  va_list list;
  __builtin_va_start(list, last);
  struct pair value = __builtin_va_arg(list, struct pair);
  int tail = __builtin_va_arg(list, int);
  __builtin_va_end(list);
  return pair.first + pair.second + (int)real + last + value.first +
         value.second + tail;
}

struct pair i386_va_sret(int first, ...) {
  va_list list;
  __builtin_va_start(list, first);
  struct pair result = {first, __builtin_va_arg(list, int)};
  __builtin_va_end(list);
  return result;
}

extern int host_va_reader(int, ...);
int i386_call_host(void) { return host_va_reader(3, -17, 19LL, 23.0); }

double i386_wide_to_double(long long value) { return (double)value; }
