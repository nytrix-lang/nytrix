long c_const_index_store(const long *items, long index, long value) {
  items[index] = value;
  return value;
}
