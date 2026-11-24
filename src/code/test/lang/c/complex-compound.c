/* Compound assignment on complex objects and implicit complex width conversion in call arguments agree with GCC. */
#include <complex.h>
#include <stdio.h>
double _Complex f1(double _Complex a, double _Complex b) { a -= b; a *= 2; return a; }
double _Complex f2(double _Complex a, double _Complex b) { a += b; a /= b; return a; }
double _Complex f3(double _Complex a, double x) { a *= x; a += 1; return a; }
float _Complex f4(float _Complex a, float _Complex b) { a *= b; a -= b; return a; }
double _Complex f5(double _Complex a, double _Complex b) { double _Complex t = a; t *= b + a; return t; }
static void show(double _Complex v) { printf("%.6g %.6g\n", creal(v), cimag(v)); }
int main(void) {
  double _Complex a = 1.5 + 2.5 * I, b = -0.5 + 4.0 * I;
  show(f1(a, b)); show(f2(a, b)); show(f3(a, 3.0)); show(f4(1.0f + 2.0f * I, 3.0f - 1.0f * I)); show(f5(a, b));
  return 0;
}
