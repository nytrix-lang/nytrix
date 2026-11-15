/*
 * Linux-style inline assembly fixture for atomic add and sign detection.
 */
static void atomic_add(unsigned int *counter, int addend,
                       unsigned char *negative_out) {
  unsigned char negative = 0;
  __asm__ volatile("lock addl %[val], %[var]"
                   : [var] "+m"(*counter), "=@ccs"(negative)
                   : [val] "er"(addend)
                   : "memory");
  *negative_out = negative;
}

static void atomic_add64(unsigned long long *counter, long long addend,
                         unsigned char *negative_out) {
  unsigned char negative = 0;
  __asm__ volatile("lock addq %[val], %[var]"
                   : [var] "+m"(*counter), "=@ccs"(negative)
                   : [val] "er"(addend)
                   : "memory");
  *negative_out = negative;
}

int main(void) {
  unsigned int value = 0x7fffffffU;
  unsigned char negative = 0;
  atomic_add(&value, 1, &negative);
  if (value != 0x80000000U || negative != 1)
    return 1;
  unsigned long long value64 = 0x7fffffffffffffffULL;
  negative = 0;
  atomic_add64(&value64, 1, &negative);
  return value64 != 0x8000000000000000ULL || negative != 1;
}
