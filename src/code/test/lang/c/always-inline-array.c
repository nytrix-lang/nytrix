/*
 * A forced-inline callee with automatic arrays, inlined twice into one caller
 * and through a second static function.
 */
static inline __attribute__((always_inline)) int sum_to(int n, int bias) {
  int a[8];
  int total = bias;
  for (int i = 0; i < 8; ++i)
    a[i] = n + i;
  for (int i = 0; i < 8; ++i)
    total += a[i] * (i + 1);
  return total;
}
static int middle(int n) { return sum_to(n, 1) + sum_to(n + 2, 3); }
int always_inline_array(int n) { return middle(n) + sum_to(n * 2, 5); }
