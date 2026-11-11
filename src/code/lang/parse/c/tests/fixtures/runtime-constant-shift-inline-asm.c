/*
 * Inline assembly fixture for a runtime shift with an emitted metadata section.
 */
static unsigned int runtime_shift(unsigned int value) {
  unsigned int result = value;
  __asm__ volatile("shrl $12,%k0\n1:\n"
                   ".pushsection runtime_shift_hash_shift,\"a\"\n\t"
                   ".long 1b - 1 - .\n"
                   ".popsection"
                   : "+r"(result));
  return result;
}

int main(void) { return runtime_shift(0x12345678u) != 0x12345u; }
