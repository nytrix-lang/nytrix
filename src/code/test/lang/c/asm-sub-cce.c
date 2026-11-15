/*
 * Fixture helper: expose the carry/equality condition from an atomic subtract.
 */
unsigned char sub_and_test_asm(long *pointer, long value) {
  unsigned char zero;
  __asm__ volatile("lock subq %[value], %[memory]"
                   : [memory] "+m"(*pointer), "=@cce"(zero)
                   : [value] "er"(value)
                   : "memory");
  return zero;
}
void addl_asm(int *pointer, unsigned int value) {
  __asm__ volatile("addl %[value], %[memory]"
                   : [memory] "+m"(*pointer)
                   : [value] "ri"(value)
                   : "memory");
}
