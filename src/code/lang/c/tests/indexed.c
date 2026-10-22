long c_index_load(const long *items, long index) { return items[index]; }

long c_i64_array(const long items[], long index) { return items[index]; }

long c_i64_array_forward(const long items[], long index) {
  return c_i64_array(items, index);
}

long c_i64_array_store(long items[], long index, long value) {
  items[index] = value;
  return items[index];
}

long c_index_store(long *items, long index, long value) {
  items[index] = value;
  return items[index];
}
