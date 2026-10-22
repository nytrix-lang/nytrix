long c_if_increment(long value, long flag) {
  if (flag)
    value = value + 1;
  return value;
}

long c_double_truth(double value) {
  if (value)
    return 1;
  else
    return 0;
}

long c_if_early_return(long value) {
  if (value)
    return value;
  return 0;
}
