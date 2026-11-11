/*
 * Inline assembly fixture for trailing-zero count instruction constraints.
 */
unsigned long nyc_inline_asm_tzcnt(unsigned long value) {
  unsigned long result;
  asm volatile("tzcnt %1,%0" : "=r"(result) : "rm"(value));
  return result;
}
