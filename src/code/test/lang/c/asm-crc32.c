/*
 * Fixture: CRC32 inline-assembly lowering for all x86 operand widths.
 */
static unsigned int nyc_crc32q(unsigned int crc, unsigned long value) {
  asm("crc32q %1, %q0" : "+r"(crc) : "rm"(value));
  return crc;
}

static unsigned int nyc_crc32l(unsigned int crc, unsigned int value) {
  asm("crc32l %1, %0" : "+r"(crc) : "rm"(value));
  return crc;
}

static unsigned int nyc_crc32w(unsigned int crc, unsigned short value) {
  asm("crc32w %1, %0" : "+r"(crc) : "rm"(value));
  return crc;
}

static unsigned int nyc_crc32b(unsigned int crc, unsigned char value) {
  asm("crc32b %1, %0" : "+r"(crc) : "rm"(value));
  return crc;
}

static unsigned int nyc_crc32_reference(unsigned int crc,
                                        unsigned long value,
                                        unsigned int bits) {
  for (unsigned int bit = 0; bit < bits; ++bit) {
    unsigned int carry = (crc ^ (unsigned int)value) & 1;
    crc = (crc >> 1) ^ (0x82f63b78u & (0u - carry));
    value >>= 1;
  }
  return crc;
}

int main(void) {
  static const unsigned long values[] = {
      0ul,
      1ul,
      0x0123456789abcdeful,
      0xfedcba9876543210ul,
      ~0ul,
      0x8000000000000000ul,
  };
  for (unsigned int seed = 0; seed < 4; ++seed)
    for (unsigned int i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
      unsigned int crc = 0x9e3779b9u * (seed + 1);
      if (nyc_crc32q(crc, values[i]) !=
              nyc_crc32_reference(crc, values[i], 64) ||
          nyc_crc32l(crc, (unsigned int)values[i]) !=
              nyc_crc32_reference(crc, (unsigned int)values[i], 32) ||
          nyc_crc32w(crc, (unsigned short)values[i]) !=
              nyc_crc32_reference(crc, (unsigned short)values[i], 16) ||
          nyc_crc32b(crc, (unsigned char)values[i]) !=
              nyc_crc32_reference(crc, (unsigned char)values[i], 8))
        return 1;
    }
  return 0;
}
