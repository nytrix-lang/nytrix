/* File-scope complex initializers such as 1.0 + 2.0 * I fold to constants and match GCC, in scalars and arrays. */
#include <complex.h>
#include <stdio.h>
double _Complex a = 1.0 + 2.0 * I;
float _Complex b = 3.0f - 4.0f * I;
double _Complex c = (1.5 + 0.5 * I) * (2.0 - 1.0 * I);
double _Complex d = -I;
double _Complex e = 2.5;
double _Complex arr[3] = {1.0 + 1.0 * I, 2.0 * I, -3.0 - 4.0 * I};
float _Complex farr[2] = {0.5f + 1.5f * I, 7.0f};
static void show(double _Complex v) { printf("%.6g %.6g\n", creal(v), cimag(v)); }
int main(void) {
  show(a); show(b); show(c); show(d); show(e);
  for (int i = 0; i < 3; ++i) show(arr[i]);
  show(farr[0]); show(farr[1]);
  return 0;
}
