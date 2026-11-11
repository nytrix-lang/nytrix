/*
 * Fixture: capture the carry flag from a bit-test inline-assembly output.
 */
static unsigned long test_word;
static unsigned char test_bit(unsigned long bit) {
  unsigned char carry;
  __asm__ volatile("btq %2, %1"
                   : "=@ccc"(carry)
                   : "m"(test_word), "Ir"(bit)
                   : "memory");
  return carry;
}
int main(void) {
  test_word = 8;
  return test_bit(3) != 1 || test_bit(2) != 0;
}
