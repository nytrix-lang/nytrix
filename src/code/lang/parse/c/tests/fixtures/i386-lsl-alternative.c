/*
 * Fixture: parse the i386 LSL assembler alternative and register constraints.
 */
static unsigned long nyc_i386_lsl(void) {
  unsigned long result;
  __asm__ volatile("# ALT: oldinstr\n"
                   "lsl %[seg],%k[p]\n"
                   "rdpid %[p]\n"
                   ".4byte (16*32+22)\n"
                   : [p] "=r"(result)
                   : "i"(0), [seg] "r"(0x7b));
  return result;
}

int main(void) { return (int)nyc_i386_lsl(); }
