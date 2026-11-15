/*
 * Inline assembly fixture for atomic OR with early-clobber constraints.
 */
static unsigned long nyc_atomic_word;

static void nyc_atomic_or(unsigned long value) {
  asm volatile("lock orq %1, %0"
               : "+m"(nyc_atomic_word)
               : "er"(value)
               : "memory");
}

int main(void) {
  nyc_atomic_word = 0x10;
  nyc_atomic_or(0x5);
  return nyc_atomic_word != 0x15;
}
