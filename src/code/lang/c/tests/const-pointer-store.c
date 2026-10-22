long c_const_store(const long *slot, long value) {
  *slot = value;
  return value;
}
