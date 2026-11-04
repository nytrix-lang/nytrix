long c_for_scope(void) {
  for (long index = 0, weight = 1; index < 1; index = index + 1) {
    weight += index;
    if (weight < 0)
      return weight;
  }
  return weight;
}
