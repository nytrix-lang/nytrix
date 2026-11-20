/*
 * Runtime harness for integer arithmetic with SSE disabled.
 */
int nyc_no_sse_sum(const int *, int);
int nyc_no_sse_varargs(int, ...);
int main(void) {
  int values[] = {7, -3, 11, 5, 19, -31, 2, 41};
  int checksum = 0;
  for (int i = 0; i < 8; ++i)
    checksum = checksum * 3 + values[i];
  return nyc_no_sse_sum(values, 4) != 20 ||
         nyc_no_sse_varargs(8, values[0], values[1], values[2], values[3],
                            values[4], values[5], values[6],
                            values[7]) != checksum;
}
