/*
 * Parser fixture for rejecting assignment to a const-qualified parameter.
 */
long c_const_update(const long value) {
  value = value + 1;
  return value;
}
