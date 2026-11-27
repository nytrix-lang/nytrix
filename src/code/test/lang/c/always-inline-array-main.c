/*
 * Host driver for always-inline-array.c: checks the inlined results.
 */
int always_inline_array(int);
int main(void) {
  /*
   * sum_to(n,b) = b + 36n + 168
   */
  int expected = 0;
  for (int n = -3; n <= 20; ++n) {
    int e = (1 + 36 * n + 168) + (3 + 36 * (n + 2) + 168) + (5 + 36 * (n * 2) + 168);
    if (always_inline_array(n) != e)
      return 1 + (n & 31);
    expected += e;
  }
  return expected == 0;
}
