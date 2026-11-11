/*
 * Inline assembly fixtures for bit-scan-reverse at 32- and 64-bit widths.
 */
int nyc_bsrq_probe(unsigned long value) {
  int bitpos = -1;
  __asm__("bsrq %1,%q0" : "+r"(bitpos) : "rm"(value));
  return bitpos + 1;
}
int nyc_bsrl_probe(unsigned int value) {
  int bitpos;
  __asm__("bsrl %1,%0" : "=r"(bitpos) : "rm"(value), "0"(-1));
  return bitpos + 1;
}
unsigned long nyc_bsr_generic_probe(unsigned long word) {
  __asm__("bsr %1,%0" : "=r"(word) : "rm"(word));
  return word;
}
