extern unsigned long rep_stos_alternative(void *address, unsigned long length);

unsigned long nyc_clear_user(void *address, unsigned long size) {
  unsigned long current_stack_pointer = 0;
  __asm__ volatile("1:\n"
                   "# ALT: oldinstr\n771:\n\trep stosb\n772:\n"
                   "# ALT: padding\n"
                   ".skip -(((775f-774f)-(772b-771b)) > 0) * "
                   "((775f-774f)-(772b-771b)),0x90\n"
                   ".pushsection .altinstr_replacement, \"ax\"\n"
                   "# ALT: replacement\n774:\n\tcall rep_stos_alternative\n"
                   "775:\n.popsection\n"
                   : "+c"(size), "+D"(address), "+r"(current_stack_pointer)
                   : "a"(0));
  return size;
}
