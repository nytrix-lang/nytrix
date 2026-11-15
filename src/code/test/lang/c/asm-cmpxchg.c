/*
 * Inline assembly fixture for compare-and-exchange constraints and outputs.
 */
int cmpxchg_asm(int *pointer, int expected, int replacement) {
  unsigned char success;
  int previous = expected;
  __asm__ volatile(
      "lock cmpxchgl %[new], %[ptr]"
      : "=@ccz"(success), [ptr] "+m"(*pointer), [old] "+a"(previous)
      : [new] "r"(replacement)
      : "memory");
  return success ? previous : previous + 1000;
}
unsigned long cmpxchg_u32_asm(unsigned int *pointer, unsigned int expected,
                              unsigned int replacement) {
  unsigned char success;
  unsigned int previous = expected;
  __asm__ volatile(
      "lock cmpxchgl %[new], %[ptr]"
      : "=@ccz"(success), [ptr] "+m"(*pointer), [old] "+a"(previous)
      : [new] "r"(replacement)
      : "memory");
  return success ? previous : 0x100000000UL | previous;
}
void *cmpxchg_pointer_asm(void **pointer, void *expected, void *replacement) {
  unsigned char success;
  void *previous = expected;
  __asm__ volatile(
      "lock cmpxchgq %[new], %[ptr]"
      : "=@ccz"(success), [ptr] "+m"(*pointer), [old] "+a"(previous)
      : [new] "r"(replacement)
      : "memory");
  return success ? previous : (void *)((unsigned long)previous + 1);
}
unsigned long long cmpxchg_qword_classic_asm(unsigned long long *pointer,
                                             unsigned long long expected,
                                             unsigned long long replacement) {
  unsigned long long previous = expected;
  __asm__ volatile("lock cmpxchgq %2, %1"
                   : "=a"(previous), "+m"(*pointer)
                   : "r"(replacement), "0"(expected)
                   : "memory");
  return previous;
}
struct cmpxchg_object {
  unsigned long value;
};
struct cmpxchg_object *
cmpxchg_pointer_null_expected_asm(struct cmpxchg_object **pointer,
                                  struct cmpxchg_object *replacement) {
  struct cmpxchg_object *previous;
  __asm__ volatile("lock cmpxchgq %2, %1"
                   : "=a"(previous), "+m"(*pointer)
                   : "r"(replacement), "0"((void *)0)
                   : "memory");
  return previous;
}
