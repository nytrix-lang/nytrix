long c_helper(long, long);
double c_double_helper(double, double);
long c_seven_params(long, long, long, long, long, long, long);
long c_ten_params(long, long, long, long, long, long, long, long, long, long);
double c_ten_doubles(double, double, double, double, double, double, double,
                     double, double, double);

long c_call(long left, long right) { return c_helper(left, right) - 7; }

double c_double_call(double left, double right) {
  return c_double_helper(left, right) + 1.0;
}

long c_seven_call(long a, long b, long c, long d, long e, long f, long g) {
  return c_seven_params(a, b, c, d, e, f, g);
}

long c_ten_call(long a, long b, long c, long d, long e, long f, long g, long h,
                long i, long j) {
  return c_ten_params(a, b, c, d, e, f, g, h, i, j);
}

double c_ten_double_call(double a, double b, double c, double d, double e,
                         double f, double g, double h, double i, double j) {
  return c_ten_doubles(a, b, c, d, e, f, g, h, i, j);
}

long c_recursive(long value) {
  if (value < 2)
    return value;
  return value * c_recursive(value - 1);
}

long c_empty_statements(long count) {
  long sum = 0;
  ;
  while (count > 0) {
    sum += count;
    --count;
    ;
  }
  for (; count < 2; ++count)
    ;
  if (sum)
    ;
  else
    ;
  return sum;
}

long c_helper(long left, long right) { return left * 3 + right; }

double c_double_helper(double left, double right) { return left * 2.0 + right; }

long c_seven_params(long a, long b, long c, long d, long e, long f, long g) {
  return a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6 + g * 7;
}

long c_ten_params(long a, long b, long c, long d, long e, long f, long g,
                  long h, long i, long j) {
  return a + b * 3 + c * 5 + d * 7 + e * 11 + f * 13 + g * 17 + h * 19 +
         i * 23 + j * 29;
}

double c_ten_doubles(double a, double b, double c, double d, double e, double f,
                     double g, double h, double i, double j) {
  return a + b * 2.0 + c * 3.0 + d * 4.0 + e * 5.0 + f * 6.0 + g * 7.0 +
         h * 8.0 + i * 9.0 + j * 10.0;
}

double c_mixed_abi(long i0, double d0, long i1, double d1, long i2, double d2,
                   long i3, double d3, long i4, double d4, long i5, double d5,
                   long i6, double d6, double d7, double d8) {
  if (i0 != 1 || i1 != 2 || i2 != 3 || i3 != 4 || i4 != 5 || i5 != 6 || i6 != 7)
    return -1.0;
  return d0 + d1 + d2 + d3 + d4 + d5 + d6 + d7 + d8;
}

double c_mixed_abi_call(long i0, double d0, long i1, double d1, long i2,
                        double d2, long i3, double d3, long i4, double d4,
                        long i5, double d5, long i6, double d6, double d7,
                        double d8) {
  return c_mixed_abi(i0, d0, i1, d1, i2, d2, i3, d3, i4, d4, i5, d5, i6, d6, d7,
                     d8);
}
