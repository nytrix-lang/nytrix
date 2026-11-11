/*
 * Fixture: inline-assembly checksum reduction for IPv4 headers.
 */
static unsigned short nyc_ip_fast_csum(const void *iph, unsigned int ihl) {
  unsigned int sum;
  asm("  movl (%1), %0\n"
      "  subl $4, %2\n"
      "  jbe 2f\n"
      "  addl 4(%1), %0\n"
      "  adcl 8(%1), %0\n"
      "  adcl 12(%1), %0\n"
      "1: adcl 16(%1), %0\n"
      "  lea 4(%1), %1\n"
      "  decl %2\n"
      "  jne 1b\n"
      "  adcl $0, %0\n"
      "  movl %0, %2\n"
      "  shrl $16, %0\n"
      "  addw %w2, %w0\n"
      "  adcl $0, %0\n"
      "  notl %0\n"
      "2:"
      : "=r"(sum), "=r"(iph), "=r"(ihl)
      : "1"(iph), "2"(ihl)
      : "memory");
  return (unsigned short)sum;
}

static unsigned short nyc_reference_csum(const unsigned int *words,
                                         unsigned int ihl) {
  unsigned int sum = 0;
  for (unsigned int i = 0; i < ihl; ++i) {
    unsigned int next = sum + words[i];
    if (next < sum)
      ++next;
    sum = next;
  }
  sum = (sum & 0xffff) + (sum >> 16);
  sum += sum >> 16;
  return (unsigned short)~sum;
}

int main(void) {
  unsigned int words[16] = {
      0x4500003c, 0x1c464000, 0x4006b1e6, 0xc0a80001, 0xc0a80002, 0xffffffff,
      0x00000001, 0x80000000, 0x7fffffff, 0x12345678, 0x87654321, 0x01010101,
      0xfefefefe, 0xdeadbeef, 0xa5a5a5a5, 0x5a5a5a5a,
  };
  for (unsigned int words_count = 5; words_count <= 16; ++words_count)
    if (nyc_ip_fast_csum(words, words_count) !=
        nyc_reference_csum(words, words_count))
      return 1;
  return 0;
}
