static volatile int address_calls;

static __attribute__((noinline)) const char *next_address(void) {
  ++address_calls;
  return (const char *)0;
}

int main(void) {
  /*
   * Memory constraints evaluate the address once; prefetch must not load it.
   */
  __asm__ volatile("prefetcht0 %0\n\tprefetcht0 %0" : : "m"(*next_address()));
  return address_calls != 1;
}
