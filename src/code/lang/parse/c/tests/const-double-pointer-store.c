/*
 * Parser fixture for diagnostics on writes through a pointer-to-const.
 */
double c_const_f64_store(const double *slot, double value) {
  *slot = value;
  return value;
}
