/* Complex arithmetic, mixed real/complex, calls with complex arguments and results, libm calls, and casts agree with GCC. */
#include <complex.h>
#include <stdio.h>
double _Complex addc(double _Complex a, double _Complex b) { return a + b; }
double _Complex mulc(double _Complex a, double _Complex b) { return a * b; }
double _Complex divc(double _Complex a, double _Complex b) { return a / b; }
float _Complex mulf(float _Complex a, float _Complex b) { return a * b; }
double _Complex negc(double _Complex a) { return -a; }
double _Complex conjc(double _Complex a) { return conj(a); }
double partsc(double _Complex a) { return creal(a) + cimag(a) * 2.0; }
int eqc(double _Complex a, double _Complex b) { return a == b; }
int nec(double _Complex a, double _Complex b) { return a != b; }
double _Complex mix(double x, double _Complex a) { return x * a + 1; }
double _Complex fma_like(double _Complex a, double _Complex b) { return a * b + a; }
double _Complex fromparts(double r, double i) { return r + i * I; }
double _Complex conv(float _Complex a) { return (double _Complex)a; }
double _Complex chain(double _Complex a, double _Complex b) { return addc(mulc(a, b), negc(a)); }
double absc(double _Complex a) { return cabs(a); }
static void show(double _Complex v) { printf("%.6g %.6g\n", creal(v), cimag(v)); }
int main(void) {
  double _Complex a = 1.5 + 2.5 * I, b = -0.5 + 4.0 * I;
  show(addc(a, b)); show(mulc(a, b)); show(divc(a, b)); show(negc(a)); show(conjc(a));
  show(mix(3.0, a)); show(fma_like(a, b)); show(fromparts(1, 2)); show(conv(1.0f + 2.0f * I));
  show(chain(a, b));
  float _Complex f = mulf(1.0f + 2.0f * I, 3.0f - 1.0f * I);
  printf("%.6g %.6g %.6g %d %d %.6g\n", crealf(f), cimagf(f), partsc(a), eqc(a, a), nec(a, b), absc(3.0 + 4.0 * I));
  return 0;
}
