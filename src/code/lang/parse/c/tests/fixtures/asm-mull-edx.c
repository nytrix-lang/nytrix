/*
 * Fixture: exercise the x86 multiply instruction's EDX:EAX outputs.
 */
int main(void) {
  unsigned long high = 0x80000000UL;
  int low;
  unsigned long factor = 0x80000003UL;

  __asm__ volatile("mull %%edx"
                   : "=d"(high), "=&a"(low)
                   : "1"(high), "0"(factor));
  return high != 0x40000001UL || (unsigned int)low != 0x80000000U;
}
