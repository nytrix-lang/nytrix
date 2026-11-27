/*
 * Host driver for indirect-call-folding.c.
 */
long viasym(long, long), viasym2(long);
double viafloat(double);
long tramp(long a, long b) { return a * 10 + b; }
double scale(double x, long n) { return x * (double)n; }
int main(void) {
  return !(viasym(2, 3) == 23 && viasym2(5) == 5 * 10 + 3 + 5 * 10 + 4 &&
           viafloat(1.5) == 6.0);
}
