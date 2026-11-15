/*
 * Inline assembly fixture for an unlocked addition returning a condition code.
 */
static unsigned long counter;

static unsigned char add_and_test_sign(long value) {
  unsigned char sign;
  __asm__ volatile("addq %[val], %[var]"
                   : [var] "+m"(counter), "=@ccs"(sign)
                   : [val] "er"(value)
                   : "memory");
  return sign;
}

int main(void) {
  counter = 0;
  if (!add_and_test_sign(-1) || counter != (unsigned long)-1)
    return 1;
  counter = 0;
  if (add_and_test_sign(1) || counter != 1)
    return 2;
  return 0;
}
