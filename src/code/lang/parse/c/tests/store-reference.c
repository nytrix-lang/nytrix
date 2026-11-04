long c_store_reference(long *slot, long value) {
  *slot = value;
  return *slot;
}
