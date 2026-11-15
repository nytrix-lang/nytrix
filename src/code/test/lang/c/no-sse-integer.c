/*
 * C fixture for integer loops compiled without SSE support.
 */
int nyc_no_sse_sum(const int *values, int count) {
  int sum = 0;
  for (int i = 0; i < count; ++i)
    sum += values[i];
  return sum;
}
