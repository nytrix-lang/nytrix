/*
 * Inline assembly fixture combining a memory input with a register constraint.
 */
static unsigned long observed;
static void set_bit(unsigned long bit) {
  __asm__ __volatile__("lock btsq %1, %0"
                       :
                       : "m"(observed), "Ir"(bit)
                       : "memory");
}
int main(void) {
  set_bit(3);
  return observed != 8;
}
