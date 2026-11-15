/*
 * Fixture: read an x86 model-specific register with inline assembly.
 */
unsigned long long nyc_rdmsr_probe(unsigned int msr) {
  unsigned int low, high;
  __asm__ volatile("1: rdmsr\n"
                   "2:\n"
                   " .pushsection __ex_table, \"aM\", @progbits, 12\n"
                   " .balign 4\n"
                   " .long (1b) - .\n"
                   " .long (2b) - .\n"
                   " .long 9\n"
                   " .popsection\n"
                   : "=a"(low), "=d"(high)
                   : "c"(msr));
  return ((unsigned long long)high << 32) | low;
}
int nyc_rdmsr_safe_probe(unsigned int msr) {
  int error;
  unsigned long low, high;
  __asm__ volatile("1: rdmsr ; xor %[err],%[err]\n"
                   "2:\n"
                   " .pushsection __ex_table, \"aM\", @progbits, 12\n"
                   " .balign 4\n"
                   " .long (1b) - .\n"
                   " .long (2b) - .\n"
                   " .macro extable_type_reg type:req reg:req\n"
                   " .long 11 + (10 << 8)\n"
                   " .endm\n"
                   " extable_type_reg reg=%[err], type=11\n"
                   " .purgem extable_type_reg\n"
                   " .popsection\n"
                   : [err] "=r"(error), "=a"(low), "=d"(high)
                   : "c"(msr));
  return error + (int)low + (int)high;
}
int nyc_wrmsr_safe_probe(unsigned int msr, unsigned long long value) {
  int error;
  __asm__ volatile("1: wrmsr ; xor %[err],%[err]\n"
                   "2:\n"
                   " .pushsection __ex_table, \"aM\", @progbits, 12\n"
                   " .balign 4\n"
                   " .long (1b) - .\n"
                   " .long (2b) - .\n"
                   " .macro extable_type_reg type:req reg:req\n"
                   " .long 11\n"
                   " .endm\n"
                   " extable_type_reg reg=%[err], type=11\n"
                   " .purgem extable_type_reg\n"
                   " .popsection\n"
                   : [err] "=a"(error)
                   : "c"(msr), "0"((unsigned int)value),
                     "d"((unsigned int)(value >> 32))
                   : "memory");
  return error;
}
unsigned long long nyc_rdpmc_probe(int counter) {
  unsigned long low, high;
  __asm__ volatile("rdpmc" : "=a"(low), "=d"(high) : "c"(counter));
  return low | (high << 32);
}
