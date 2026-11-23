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

/* Adjacent word copies are SLP/memcombine candidates; they must stay scalar. */
void nyc_no_sse_copy4(long *restrict dst, const long *restrict src) {
  long a = src[0], b = src[1], c = src[2], d = src[3];
  dst[0] = a;
  dst[1] = b;
  dst[2] = c;
  dst[3] = d;
}
