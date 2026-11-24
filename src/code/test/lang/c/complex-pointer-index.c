/* Subscripts on complex pointers read and write the complex object at p + i. */
#include <complex.h>
#include <stdio.h>
double _Complex get(double _Complex *p, int i) { return p[i]; }
void put(double _Complex *p, int i, double _Complex v) { p[i] = v; }
double _Complex dot(double _Complex *p, double _Complex *q, int n) {
  double _Complex s = 0;
  for (int i = 0; i < n; ++i) s = s + p[i] * q[i];
  return s;
}
float _Complex getf(float _Complex *p, int i) { return p[i]; }
