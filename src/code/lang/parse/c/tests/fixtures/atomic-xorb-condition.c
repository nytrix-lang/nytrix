/*
 * Inline assembly fixture for atomic byte XOR and sign-condition output.
 */
static signed char nyc_atomic_xor_byte;
static int nyc_atomic_xor_sign(unsigned long mask,
                               volatile unsigned long *addr) {
  unsigned char negative;
  asm volatile("lock "
               "xorb %2,%1"
               : "=@ccs"(negative), "+m"(*(volatile char *)(addr))
               : "iq"((char)mask)
               : "memory");
  return negative;
}
int main(void) {
  volatile unsigned long *addr = (volatile unsigned long *)&nyc_atomic_xor_byte;
  if (!nyc_atomic_xor_sign(0x80, addr) || nyc_atomic_xor_byte != -128)
    return 1;
  if (nyc_atomic_xor_sign(0x80, addr) || nyc_atomic_xor_byte != 0)
    return 2;
  return 0;
}
