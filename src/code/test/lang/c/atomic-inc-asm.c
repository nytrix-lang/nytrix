/*
 * Fixture: increment shared storage with a locked inline-assembly instruction.
 */
static unsigned long nyc_atomic_counter;
int main(void) {
  asm volatile("lock incq %0"
               : "=m"(nyc_atomic_counter)
               : "m"(nyc_atomic_counter)
               : "memory");
  return nyc_atomic_counter != 1;
}
