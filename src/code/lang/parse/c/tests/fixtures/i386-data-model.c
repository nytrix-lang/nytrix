/*
 * Fixture: assert the i386 widths of pointers, integers, and size_t.
 */
_Static_assert(sizeof(long) == 4, "i386 long is 32 bits");
_Static_assert(sizeof(long long) == 8, "i386 long long is 64 bits");
_Static_assert(sizeof(void *) == 4, "i386 pointers are 32 bits");
_Static_assert(sizeof(size_t) == 4, "i386 size_t is 32 bits");
_Static_assert(sizeof(int *) == 4, "i386 object pointers are 32 bits");
_Static_assert(__alignof__(long long) == 4, "i386 long long alignment");
_Static_assert(__alignof__(void *) == 4, "i386 pointer alignment");

register unsigned long current_stack_pointer __asm__("esp");

unsigned int nyc_i386_hweight32(unsigned int value) {
  unsigned int result;
  asm volatile("# ALT: oldinstr\n\tcall __sw_hweight32\n"
               "# ALT: replacement\n\tpopcntl %[val], %[cnt]"
               : [cnt] "=a"(result), "+r"(current_stack_pointer)
               : [val] "a"(value));
  return result;
}

int __attribute__((regparm(0))) nyc_i386_data_model_probe(void) {
  return sizeof(long) + sizeof(long long) + sizeof(void *);
}

unsigned long nyc_i386_label_address_probe(void) {
  unsigned long address = (unsigned long)&&here;
here:
  return address;
}
