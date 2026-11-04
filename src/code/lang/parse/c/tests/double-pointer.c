double c_f64_load(const double *pointer) { return *pointer; }

double c_f64_store(double *slot, double value) {
  *slot = value;
  return *slot;
}

double c_f64_index(const double *items, long index) { return items[index]; }

double c_f64_array(const double items[], long index) { return items[index]; }

double c_f64_array_forward(const double items[], long index) {
  return c_f64_array(items, index);
}

double c_f64_array_store(double items[], long index, double value) {
  items[index] = value;
  return items[index];
}

double c_f64_index_store(double *items, long index, double value) {
  items[index] = value;
  return items[index];
}
