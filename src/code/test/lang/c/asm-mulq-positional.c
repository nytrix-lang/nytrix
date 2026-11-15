/*
 * Fixture: compile positional operand constraints for x86-64 inline assembly.
 */
int main(void) {
  unsigned long random = ~0UL;
  unsigned long raw;
  unsigned long multiplier = 2;
  __asm__ volatile(" mulq %3"
                   : "=a"(random), "=d"(raw)
                   : "a"(random), "rm"(multiplier));
  return random != ~1UL || raw != 1UL;
}
