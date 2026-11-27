/*
 * Local arrays of complex values: initializers, element assignment, compound assignment and pointer stores agree with GCC.
 */
#include <complex.h>
#include <stdio.h>
double _Complex sum(int n) {
  double _Complex a[4] = {1.0, 2.0 * I, 3.0 + 1.0 * I, -1.0};
  double _Complex s = 0;
  for (int i = 0; i < n; ++i) s = s + a[i] * a[i];
  return s;
}
double _Complex fill(int i) {
  double _Complex a[3];
  a[0] = 1;
  a[1] = 2.0 * I;
  a[2] = a[0] + a[1];
  a[2] *= a[1];
  a[0] -= 0.5;
  return a[i];
}
float _Complex single(int i) {
  float _Complex b[2] = {1.0f, 2.0f * I};
  b[1] += b[0];
  return b[i];
}
double _Complex through(double _Complex *p, int n) {
  for (int i = 1; i < n; ++i) p[i] = p[i - 1] * 2.0;
  p[0] += p[n - 1];
  return p[0];
}
static void show(double _Complex v) { printf("%.6g %.6g\n", creal(v), cimag(v)); }
int main(void) {
  show(sum(4)); show(fill(0)); show(fill(1)); show(fill(2)); show(single(0)); show(single(1));
  double _Complex q[3] = {1.0 + 1.0 * I, 0, 0};
  show(through(q, 3)); show(q[2]);
  return 0;
}
