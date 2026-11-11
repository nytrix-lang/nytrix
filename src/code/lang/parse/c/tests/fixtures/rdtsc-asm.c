/*
 * Inline assembly fixture for RDTSC with kernel-style alternatives.
 */
static unsigned long read_tsc(void) {
  unsigned int low, high;
  asm volatile("# ALT: oldinstr\nrdtsc\n"
               "# ALT: replacement\nlfence; rdtsc\n"
               "# ALT: replacement\nrdtscp\n"
               ".4byte (20*32+ 2)\n.4byte (1*32+27)"
               : "=a"(low), "=d"(high)
               :
               : "ecx");
  return ((unsigned long)high << 32) | low;
}
int main(void) { return read_tsc() == 0; }
