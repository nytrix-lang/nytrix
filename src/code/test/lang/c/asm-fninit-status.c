/*
 * C front-end regression test: asm fninit status.
 */
int main(void) {
  unsigned long status = 0xa5a5ffffUL;
  unsigned long control = 0x5a5affffUL;
  __asm__ volatile("fninit ; fnstsw %0 ; fnstcw %1"
                   : "+m"(status), "+m"(control));
  return status != 0xa5a50000UL || control != 0x5a5a037fUL;
}
