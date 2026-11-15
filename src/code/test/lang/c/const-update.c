/*
 * Parser fixture for rejecting assignment to a const-qualified parameter.
 */
long c_const_update(const long value) {
  value = value + 1;
  return value;
}

long c_const_expression_update(void) {
  const long value = 3;
  return (value = 4);
}
