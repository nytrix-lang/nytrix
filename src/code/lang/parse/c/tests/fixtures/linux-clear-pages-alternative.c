extern void *memset(void *destination, int value, unsigned long size);

static void clear_pages(void *address, unsigned long length) {
  unsigned long current_stack_pointer = 0;
  asm volatile("# ALT: oldinstr\n"
               "771:\n\tcall __clear_pages_unrolled\n772:\n"
               "# ALT: padding\n"
               ".skip -(((775f-774f)-(772b-771b)) > 0) * "
               "((775f-774f)-(772b-771b)),0x90\n"
               "773:\n.pushsection .altinstructions, \"aM\", @progbits, 14\n"
               " .long 771b - .\n .long 774f - .\n"
               " .4byte (3*32+16)\n.popsection\n"
               ".pushsection .altinstr_replacement, \"ax\"\n"
               "# ALT: replacement\n774:\n\tshrq $3, %%rcx; rep stosq\n"
               "775:\n.popsection\n"
               "# ALT: oldinstr\n772:\n# ALT: padding\n"
               ".skip -(((775f-774f)-(772b-771b)) > 0) * "
               "((775f-774f)-(772b-771b)),0x90\n"
               ".pushsection .altinstr_replacement, \"ax\"\n"
               "# ALT: replacement\n774:\n\trep stosb\n775:\n.popsection\n"
               : "+c"(length), "+D"(address), "+r"(current_stack_pointer)
               : "a"(0)
               : "cc", "memory");
}

int main(void) {
  unsigned long page[8];
  for (unsigned long i = 0; i < 8; ++i)
    page[i] = 0x123456789abcdef0UL;
  clear_pages(page, sizeof(page));
  for (unsigned long i = 0; i < 8; ++i)
    if (page[i] != 0)
      return 1;
  return 0;
}
