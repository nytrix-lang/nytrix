/*
 * Fixture: store a user value and recover through a fault-handling branch.
 */
int main(void) {
  unsigned long stored = 0;
  unsigned long value = 0x123456789abcdef0UL;
  unsigned long *target = &stored;
  __asm__ goto("1:\tmovq %0,%1\n"
               " .pushsection __ex_table, \"aM\", @progbits, 12\n"
               " .balign 4\n"
               " .long (1b) - .\n"
               " .long (%l2) - .\n"
               " .long 3\n"
               " .popsection\n"
               :
               : "iq"(value), "m"(*target)
               :
               : fault);
  return stored != value;
fault:
  return 1;
}
