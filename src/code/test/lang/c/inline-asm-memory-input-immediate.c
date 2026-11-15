/*
 * Fixture helper: load a character through a memory-constrained operand.
 */
static unsigned char bits;

int nyc_inline_asm_memory_input(const char *value) {
  bits = (unsigned char)*value;
  unsigned char matches;
  __asm__ volatile("testb %2, %1"
                   : "=@ccnz"(matches)
                   : "m"(bits), "i"(65)
                   : "cc", "memory");
  return matches ? 65 : 0;
}
