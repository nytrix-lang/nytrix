double c_double(double left, double right) { return left * 2.0 + right; }

double c_double_choose(double left, double right) {
  if (left < right)
    return left;
  else
    return right;
}
