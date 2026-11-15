/*
 * Fixture: implement an atomic spinlock increment with x86 XADD.
 */
static unsigned long add_return(unsigned long *counter, unsigned long value) {
  __asm__ volatile("xaddq %[tmp], %[var]"
                   : [tmp] "+r"(value), [var] "+m"(*counter)
                   :
                   : "memory");
  return value;
}

int main(void) {
  unsigned long counter = 10;
  unsigned long previous = add_return(&counter, 3);
  return previous != 10 || counter != 13;
}
