/*
 * Reference fixture: store through a pointer and return the stored value.
 */
long c_store_reference(long *slot, long value) {
  *slot = value;
  return *slot;
}
