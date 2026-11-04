long c_while_sum(long count) {
  long index = 0;
  long sum = 0;
  while (index < count) {
    sum = sum + index;
    index = index + 1;
  }
  return sum;
}

long c_while_early_return(long count) {
  while (count > 0)
    return count + 7;
  return -1;
}

long c_while_continue(long count) {
  long index = 0;
  long sum = 0;
  while (index < count) {
    index = index + 1;
    if (index == 2)
      continue;
    sum = sum + index;
  }
  return sum;
}

long c_while_break(long count) {
  long index = 0;
  long sum = 0;
  while (index < count) {
    index = index + 1;
    if (index == 3)
      break;
    sum = sum + index;
  }
  return sum;
}

long c_nested_while_control(long count) {
  long outer = 0;
  long sum = 0;
  while (outer < count) {
    long inner = 0;
    while (inner < count) {
      inner = inner + 1;
      if (inner == 2)
        continue;
      if (inner == 4)
        break;
      sum = sum + outer + inner;
    }
    outer = outer + 1;
  }
  return sum;
}
