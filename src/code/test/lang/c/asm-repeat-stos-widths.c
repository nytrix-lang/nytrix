/*
 * Fixture: test repeat-store inline assembly across supported operand widths.
 */
static void fill_bytes(unsigned char *dst, unsigned long count,
                       unsigned char value) {
  __asm__ volatile("rep stosb"
                   : "+D"(dst), "+c"(count)
                   : "a"(value)
                   : "memory");
}

static void fill_words(unsigned short *dst, unsigned long count,
                       unsigned short value) {
  __asm__ volatile("rep stosw"
                   : "+D"(dst), "+c"(count)
                   : "a"(value)
                   : "memory");
}

static void fill_dwords(unsigned int *dst, unsigned long count,
                        unsigned int value) {
  __asm__ volatile("rep stosl"
                   : "+D"(dst), "+c"(count)
                   : "a"(value)
                   : "memory");
}

static void fill_qwords(unsigned long *dst, unsigned long count,
                        unsigned long value) {
  __asm__ volatile("rep stosq"
                   : "+D"(dst), "+c"(count)
                   : "a"(value)
                   : "memory");
}

int main(void) {
  unsigned char bytes[2];
  unsigned short words[2];
  unsigned int dwords[2];
  unsigned long qwords[2];
  fill_bytes(bytes, 2, 0xa5);
  fill_words(words, 2, 0x5aa5);
  fill_dwords(dwords, 2, 0x5aa55aa5U);
  fill_qwords(qwords, 2, 0x5aa55aa55aa55aa5UL);
  return bytes[0] != 0xa5 || bytes[1] != 0xa5 || words[0] != 0x5aa5 ||
         words[1] != 0x5aa5 || dwords[0] != 0x5aa55aa5U ||
         dwords[1] != 0x5aa55aa5U || qwords[0] != 0x5aa55aa55aa55aa5UL ||
         qwords[1] != 0x5aa55aa55aa55aa5UL;
}
