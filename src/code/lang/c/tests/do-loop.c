long c_do_sum(long count) {
  long index = 0;
  long sum = 0;
  do {
    if (index == 2) {
      index++;
      continue;
    }
    sum = sum + index;
    index++;
  } while (index < count);
  return sum;
}

long c_do_break(long count) {
  long index = 0;
  long sum = 0;
  do {
    index++;
    if (index == 3)
      break;
    sum = sum + index;
  } while (index < count);
  return sum;
}

long c_do_nested_conditional(long count) {
  long iterations = 0;
  if (count > 0)
    do {
      iterations++;
    } while (iterations < count);
  return iterations;
}
