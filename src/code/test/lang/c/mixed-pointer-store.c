/*
 * Parser fixture: store a floating-point value through an integer pointer.
 */
long c_mixed_store(long *slot, double value) {
  *slot = value;
  return 0;
}
