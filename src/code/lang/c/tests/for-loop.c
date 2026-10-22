long c_for_sum(long count) {
  long sum = 0;
  for (long index = 0; index < count; index = index + 1)
    sum = sum + index;
  return sum;
}

long c_for_multi_declarators(long count) {
  long sum = 0;
  for (long index = 0, weight = 1; index < count; ++index) {
    sum += index * weight;
    ++weight;
  }
  return sum;
}

long c_for_assign_sum(long count) {
  long index = 0;
  long sum = 0;
  for (index = 0; index < count; index = index + 1)
    sum = sum + index;
  return sum;
}

long c_for_body_step(long count) {
  long index = 0;
  long sum = 0;
  for (; index < count;) {
    sum = sum + index;
    index = index + 1;
  }
  return sum;
}

long c_for_early_return(long count) {
  for (long index = 0; index < count; index = index + 1)
    return index + count;
  return -1;
}

long c_for_shadow(long seed) {
  long index = seed;
  for (long index = 0; index < 3; index = index + 1)
    seed = seed + index;
  return seed + index;
}

long c_nested_for_sum(long count) {
  long sum = 0;
  for (long outer = 0; outer < count; outer = outer + 1) {
    for (long inner = 0; inner < count; inner = inner + 1)
      sum = sum + outer + inner;
  }
  return sum;
}

long c_for_continue(long count) {
  long sum = 0;
  for (long index = 0; index < count; index = index + 1) {
    if (index == 2)
      continue;
    sum = sum + index;
  }
  return sum;
}

long c_for_break(long count) {
  long sum = 0;
  for (long index = 0; index < count; index = index + 1) {
    if (index == 3)
      break;
    sum = sum + index;
  }
  return sum;
}

long c_nested_for_control(long count) {
  long sum = 0;
  for (long outer = 0; outer < count; outer = outer + 1) {
    for (long inner = 0; inner < count; inner = inner + 1) {
      if (inner == 1)
        continue;
      if (inner == 3)
        break;
      sum = sum + outer + inner;
    }
  }
  return sum;
}
