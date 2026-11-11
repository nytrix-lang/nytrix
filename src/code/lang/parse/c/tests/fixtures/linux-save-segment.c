/*
 * Fixture: read an x86 segment selector using inline assembly.
 */
void nyc_save_ds(void) {
  unsigned short value;
  __asm__ volatile("movl %%ds,%k0" : "=r"(value));
}
