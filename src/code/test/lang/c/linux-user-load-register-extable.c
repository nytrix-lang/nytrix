/*
 * Fixture: load user memory through a register operand and exception table.
 */
struct nyc_large_user {
  unsigned long value;
  unsigned long padding[99];
};

static unsigned long nyc_load_u16(const struct nyc_large_user *source,
                                  int *faulted) {
  unsigned long value = 0;
  int error = 0;
  __asm__ volatile("\n1:\tmovw %[umem],%[output]\n"
                   "2:\n"
                   ".pushsection __ex_table, \"aM\", @progbits, 12\n"
                   ".balign 4\n"
                   ".long (1b) - .\n"
                   ".long (2b) - .\n"
                   ".long 0xfff21a11\n"
                   ".popsection\n"
                   : [errout] "=r"(error), [output] "=a"(value)
                   : [umem] "m"(*source), "0"(error));
  if (error)
    *faulted = 1;
  return value;
}

int main(void) {
  unsigned long source_value = 0x123456789abcdef0UL;
  const struct nyc_large_user *source =
      (const struct nyc_large_user *)&source_value;
  int faulted = 0;
  return nyc_load_u16(source, &faulted) != 0xdef0 || faulted;
}
