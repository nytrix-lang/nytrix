/*
 * Fixture: load user memory with an exception-table recovery branch.
 */
struct nyc_faultable_user {
  unsigned long value;
  unsigned long padding[99];
};

#define DEFINE_FAULTABLE_LOAD(name, type, instruction, constraint)     \
  static type nyc_load_##name(const struct nyc_faultable_user *source, \
                              int *faulted) {                          \
    type value = 0;                                                    \
    asm goto("\n1:\t" #instruction " %[umem],%[output]\n"              \
             "2:\n"                                                    \
             ".pushsection __ex_table, \"aM\", @progbits, 12\n"        \
             ".balign 4\n"                                             \
             ".long (1b) - .\n"                                        \
             ".long (%l2) - .\n"                                       \
             ".long 3\n"                                               \
             ".popsection\n"                                           \
             : [output] constraint(value)                              \
             : [umem] "m"(*source)                                     \
             :                                                         \
             : fault);                                                 \
    return value;                                                      \
  fault:                                                               \
    *faulted = 1;                                                      \
    return 0;                                                          \
  }

DEFINE_FAULTABLE_LOAD(u8, unsigned char, movb, "=q")
DEFINE_FAULTABLE_LOAD(u16, unsigned short, movw, "=r")
DEFINE_FAULTABLE_LOAD(u32, unsigned int, movl, "=r")
DEFINE_FAULTABLE_LOAD(u64, unsigned long, movq, "=r")

static void nyc_load_u64_to_pointer(const unsigned long *source,
                                    unsigned long *destination, int *faulted) {
  asm goto("\n1:\tmovq %[umem],%[output]\n"
           "2:\n"
           ".pushsection __ex_table, \"aM\", @progbits, 12\n"
           ".balign 4\n"
           ".long (1b) - .\n"
           ".long (%l2) - .\n"
           ".long 3\n"
           ".popsection\n"
           : [output] "=r"(*destination)
           : [umem] "m"(*source)
           :
           : fault);
  return;
fault:
  *faulted = 1;
}

int main(void) {
  unsigned long source_value = 0x123456789abcdef0UL;
  const struct nyc_faultable_user *source =
      (const struct nyc_faultable_user *)&source_value;
  unsigned long output = 0;
  unsigned char value8 = 0xf0;
  unsigned short value16 = 0xdef0;
  unsigned int value32 = 0x9abcdef0;
  int faulted = 0;
  nyc_load_u64_to_pointer((const unsigned long *)&source_value, &output,
                          &faulted);
  return nyc_load_u8(source, &faulted) != value8 ||
         nyc_load_u16(source, &faulted) != value16 ||
         nyc_load_u32(source, &faulted) != value32 ||
         nyc_load_u64(source, &faulted) != source_value ||
         output != source_value || faulted;
}
