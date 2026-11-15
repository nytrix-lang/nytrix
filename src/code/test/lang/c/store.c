/*
 * Parser fixture: store a value through a pointer and read it back.
 */
long c_store(long *slot, long value) {
  *slot = value;
  return *slot;
}
