void nyc_save_ds(void) {
  unsigned short value;
  __asm__ volatile("movl %%ds,%k0" : "=r"(value));
}
