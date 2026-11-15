/*
 * Fixture: bind inline-assembly inputs to fixed x86-64 registers.
 */
unsigned long nyc_fixed_register_a;
unsigned long nyc_fixed_register_c;
unsigned long nyc_fixed_register_d;
void nyc_fixed_register_inputs(unsigned long a, unsigned long c,
                               unsigned long d) {
  asm volatile("movq %%rax, nyc_fixed_register_a(%%rip)\n\t"
               "movq %%rcx, nyc_fixed_register_c(%%rip)\n\t"
               "movq %%rdx, nyc_fixed_register_d(%%rip)"
               :
               : "a"(a), "c"(c), "d"(d)
               : "memory");
}
