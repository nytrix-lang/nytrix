/*
 * C fixture for reading a global register variable through inferred types.
 */
register unsigned long cur asm("rsp");
unsigned long nyc_register_support(void) {
  typeof(cur) value = cur;
  __typeof__(cur) copy = value;
  return copy & 15;
}
