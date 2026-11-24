/* File-scope float, double and complex objects and arrays: initializers, element reads and writes, and compound assignment agree with GCC. */
#include <complex.h>
#include <stdio.h>
double dg[3] = {1.5, 2.5, 3.5};
float fg[4] = {1.0f, 2.0f};
static double grid[2][2] = {{1, 2}, {3, 4}};
double _Complex zs = 2.0;
float _Complex zf = 3.0f;
double _Complex za[3] = {1.0, 2.0, 4.0};
double _Complex zu[2];
double sum(void) { double s = 0; for (int i = 0; i < 3; ++i) s += dg[i]; return s + fg[1] + grid[1][0]; }
void update(int i, double v) { dg[i] = v; fg[i] = (float)v; dg[0] += v; }
double _Complex zmix(int i) {
  zu[i] = za[i] * zs;
  zu[0] += za[2];
  zs += 1.0;
  zs *= za[1];
  zf = zf * 2.0f;
  return zu[i] + zs + zf;
}
static void show(double _Complex v) { printf("%.6g %.6g\n", creal(v), cimag(v)); }
int main(void) {
  printf("%.6g\n", sum());
  update(1, 7.25);
  printf("%.6g %.6g %.6g\n", sum(), dg[1], (double)fg[1]);
  show(zmix(1)); show(zmix(0)); show(zu[0]); show(zs); show(zf);
  return 0;
}
