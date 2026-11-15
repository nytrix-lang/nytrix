/*
 * Fixture: lower the kernel's immediate-and-NOT ENDBR signature sequence.
 */
int main(void) {
  unsigned int signature;
  __asm__ volatile("mov $~0xfa1e0ff3, %[endbr]\n\t"
                   "not %[endbr]\n\t"
                   : [endbr] "=&r"(signature));
  return signature != 0xfa1e0ff3u;
}
