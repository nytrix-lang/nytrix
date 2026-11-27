/*
 * Calls through the address of a named function fold to direct calls.
 */
extern long tramp(long, long);
long viasym(long a, long b) { return ((long (*)(long, long))tramp)(a, b); }
long viasym2(long a) { long (*f)(long, long) = &tramp; return f(a, 3) + f(a, 4); }
extern double scale(double, long);
double viafloat(double x) { return ((double (*)(double, long))scale)(x, 4); }
