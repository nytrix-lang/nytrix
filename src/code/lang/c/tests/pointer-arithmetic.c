long c_pointer_add(const long *items, long index) { return *(items + index); }

long c_pointer_add_reversed(const long *items, long index) {
  return *(index + items);
}

long c_pointer_subtract(const long *items, long index) {
  return *(items + 4 - index);
}

long c_pointer_compound_offset(long *items, long index) {
  long *current = items;
  current += index;
  long selected = *current;
  current -= index;
  return selected + *current;
}

long c_pointer_postfix_increment(long *items) {
  long *current = items;
  long previous = *current++;
  return previous + *current;
}

long c_pointer_prefix_increment(long *items) {
  long *current = items;
  return *++current;
}

long c_pointer_prefix_decrement(long *items) {
  long *current = items + 1;
  return *--current;
}

long c_const_data_pointer_increment(const long *items) {
  const long *current = items;
  long previous = *current++;
  current = items;
  current += 1;
  return previous + *current;
}

double c_double_pointer_add(const double *items, long index) {
  return *(items + index);
}

double c_double_pointer_compound_offset(double *items, long index) {
  double *current = items;
  current += index;
  double selected = *current;
  current -= index;
  return selected + *current;
}

double c_double_pointer_postfix_increment(double *items) {
  double *current = items;
  double previous = *current++;
  return previous + *current;
}
