unsigned int __sw_hweight32(unsigned int);
unsigned long __sw_hweight64(unsigned long);
unsigned long nyc_hweight64_probe(unsigned long value) {
  unsigned long result;
  unsigned long current_stack_pointer = 0;
  __asm__("# ALT: oldinstr\n"
          "771:\n\tcall __sw_hweight64\n772:\n"
          ".pushsection .altinstructions,\"aM\",@progbits,14\n"
          ".long 771b - .\n.long 774f - .\n.4byte 151\n"
          ".byte 772b - 771b\n.byte 775f - 774f\n.popsection\n"
          ".pushsection .altinstr_replacement,\"ax\"\n"
          "# ALT: replacement\n774:\n\tpopcntq %[val], %[cnt]\n775:\n"
          ".popsection\n"
          : [cnt] "=a"(result), "+r"(current_stack_pointer)
          : [val] "D"(value));
  return result;
}
unsigned int nyc_hweight32_probe(unsigned int value) {
  unsigned int result;
  unsigned int current_stack_pointer = 0;
  __asm__("# ALT: oldinstr\n"
          "771:\n\tcall __sw_hweight32\n772:\n"
          ".pushsection .altinstructions,\"aM\",@progbits,14\n"
          ".long 771b - .\n.long 774f - .\n.4byte 151\n"
          ".byte 772b - 771b\n.byte 775f - 774f\n.popsection\n"
          ".pushsection .altinstr_replacement,\"ax\"\n"
          "# ALT: replacement\n774:\n\tpopcntl %[val], %[cnt]\n775:\n"
          ".popsection\n"
          : [cnt] "=a"(result), "+r"(current_stack_pointer)
          : [val] "D"(value));
  return result;
}
