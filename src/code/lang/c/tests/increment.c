long c_prefix_increment(long value) { return ++value; }

long c_prefix_decrement(long value) { return --value; }

long c_postfix_update(long value) {
  long previous = value++;
  return previous * 10 + value;
}

long c_postfix_decrement(long value) {
  long previous = value--;
  return previous * 10 + value;
}

long c_for_increment(long count) {
  long sum = 0;
  for (long index = 0; index < count; ++index)
    sum = sum + index;
  return sum;
}

long c_while_increment(long count) {
  long index = 0;
  long sum = 0;
  while (index < count) {
    sum = sum + index;
    index++;
  }
  return sum;
}

double c_double_postfix(double value) {
  double previous = value++;
  return previous + value;
}

long c_const_postfix_increment(void) {
  const long value = 3;
  return value++;
}

long c_const_prefix_decrement(void) {
  const long value = 3;
  return --value;
}
