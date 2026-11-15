/*
 * Inline assembly fixture for a condition output paired with memory operands.
 */
static unsigned char bits = 8;
static unsigned char test_bit(unsigned long bit) {
  unsigned char set;
  __asm__ volatile("testb %2, %1"
                   : "=@ccnz"(set)
                   : "m"(bits), "i"(1u << (bit & 7))
                   : "memory");
  return set;
}
int main(void) { return test_bit(3) != 1 || test_bit(2) != 0; }
