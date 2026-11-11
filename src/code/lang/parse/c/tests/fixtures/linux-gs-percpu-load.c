/*
 * Fixture: load a per-CPU value through the x86 GS segment register.
 */
int nyc_linux_gs_percpu_load(unsigned int *address) {
  int value;
  __asm__ __volatile__("movl %%gs:%[var], %[val]"
                       : [val] "=r"(value)
                       : [var] "m"(*address));
  return value;
}
