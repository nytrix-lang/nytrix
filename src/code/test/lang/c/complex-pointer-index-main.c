/*
 * Host driver for complex-pointer-index.c; its output is compared with complex-pointer-index.expected.
 */
#include <complex.h>
#include <stdio.h>
double _Complex get(double _Complex *, int); void put(double _Complex *, int, double _Complex); double _Complex dot(double _Complex *, double _Complex *, int); float _Complex getf(float _Complex *, int);
static void show(double _Complex v) { printf("%.6g %.6g\n", creal(v), cimag(v)); }
int main(void) {
  double _Complex a[3] = {1.0 + 2.0 * I, 3.0 - 1.0 * I, -2.0 + 0.5 * I}, b[3] = {0.5 * I, 2.0, 1.0 + 1.0 * I};
  show(get(a, 1)); put(b, 0, 7.0 - 3.0 * I); show(b[0]); show(dot(a, b, 3));
  float _Complex f[2] = {1.0f + 2.0f * I, 3.0f + 4.0f * I}; float _Complex g = getf(f, 1); printf("%.6g %.6g\n", crealf(g), cimagf(g));
  return 0;
}
