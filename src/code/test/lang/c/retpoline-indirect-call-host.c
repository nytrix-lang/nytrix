/*
 * Runtime harness for indirect calls routed through a retpoline fixture.
 */
#include <stdarg.h>

long retpoline_target(long);
long retpoline_call(long (*)(long), long);
double retpoline_variadic_call(double (*)(int, ...), double);

static double sum(int n, ...) {
  va_list args;
  va_start(args, n);
  double result = 0;
  for (int i = 0; i < n; ++i)
    result += va_arg(args, double);
  va_end(args);
  return result;
}

int main(void) {
  return retpoline_call(retpoline_target, 9) != 28 ||
         retpoline_variadic_call(sum, 3.5) != 4.75;
}
