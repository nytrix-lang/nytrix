/*
 * C front-end regression test: asm fixed r9.
 */
__attribute__((noinline)) void fault_in(const unsigned long *address) {
  __asm__ volatile("mov %[address], %%r9"
                   :
                   : [address] "g"(*address)
                   : "r9", "memory");
}
