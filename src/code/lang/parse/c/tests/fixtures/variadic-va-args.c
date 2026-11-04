typedef __builtin_va_list va_list;
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_arg(ap, type) __builtin_va_arg(ap, type)
struct va_list_pointer_holder {
  va_list *list;
};
static double read_variadic(int tag, ...) {
  va_list args;
  va_start(args, tag);
  int integer = va_arg(args, int);
  double real = va_arg(args, double);
  return tag + integer + real;
}
static int read_variadic_stack(int tag, ...) {
  va_list args;
  va_start(args, tag);
  int total = tag;
  for (int i = 0; i < 100; ++i)
    total += i;
  for (int i = 0; i < 6; ++i)
    total += va_arg(args, int);
  return total;
}
static int read_variadic_indirect(int tag, ...) {
  va_list args;
  va_start(args, tag);
  va_list *cursor = &args;
  return va_arg(*cursor, int);
}
static int read_variadic_copy(int tag, ...) {
  va_list args, copy;
  va_start(args, tag);
  __builtin_va_copy(copy, args);
  return va_arg(copy, int);
}
static int read_variadic_copy_through_field(int tag, ...) {
  va_list args, copy;
  va_start(args, tag);
  struct va_list_pointer_holder holder = {.list = &args};
  __builtin_va_copy(copy, *holder.list);
  return va_arg(copy, int);
}
int main(void) {
  return read_variadic(2, 7, 0.5) != 9.5 ||
         read_variadic_stack(1, 1, 2, 3, 4, 5, 6) != 4972 ||
         read_variadic_indirect(0, 19) != 19 ||
         read_variadic_copy(0, 23) != 23 ||
         read_variadic_copy_through_field(0, 29) != 29;
}
