long c_compound(long value) {
  long output = value;
  {
    long temporary = output + 3;
    output = temporary * 2;
  }
  return output;
}

long c_compound_assignments(long value) {
  long result = value;
  result += 7;
  result -= 2;
  result *= 3;
  result /= 3;
  result %= 11;
  result &= 15;
  result |= 2;
  result ^= 1;
  return result;
}

double c_double_compound_assignment(double value) {
  double result = value;
  result += 1.5;
  result *= 2.0;
  result -= 0.5;
  result /= 2.0;
  return result;
}

long c_pointer_compound_assignment(long *slot, long value) {
  *slot += value;
  return *slot;
}

long c_index_compound_assignment(long *items, long index, long value) {
  items[index] ^= value;
  return items[index];
}

double c_double_pointer_compound_assignment(double *slot, double value) {
  *slot *= value;
  return *slot;
}

long c_pointer_compound_unsupported(long *pointer) {
  long *local = pointer;
  local *= 2;
  return *local;
}

long c_block_scope(void) {
  {
    long hidden = 4;
  }
  return hidden;
}

long c_shadow_local(long value) {
  long shadow = value;
  {
    long shadow = 3;
    value = value + shadow;
  }
  return shadow + value;
}

long c_redeclare_local(void) {
  long value = 1;
  long value = 2;
  return value;
}
