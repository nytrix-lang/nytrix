/*
 * C fixture for integer loops compiled without SSE support.
 */
int nyc_no_sse_sum(const int *values, int count) {
  int sum = 0;
  for (int i = 0; i < count; ++i)
    sum += values[i];
  return sum;
}

typedef __builtin_va_list va_list;

int nyc_no_sse_varargs(int count, ...) {
  va_list args;
  __builtin_va_start(args, count);
  int checksum = 0;
  for (int i = 0; i < count; ++i)
    checksum = checksum * 3 + __builtin_va_arg(args, int);
  __builtin_va_end(args);
  return checksum;
}
