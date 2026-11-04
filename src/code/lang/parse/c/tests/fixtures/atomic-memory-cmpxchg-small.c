static unsigned char cmpxchg_byte(unsigned char *memory, unsigned char expected,
                                  unsigned char replacement) {
  unsigned char old;
  asm volatile("lock cmpxchgb %2, %1"
               : "=a"(old), "+m"(*memory)
               : "q"(replacement), "0"(expected)
               : "memory");
  return old;
}

static short cmpxchg_word(short *memory, short expected, short replacement) {
  short old;
  asm volatile("lock cmpxchgw %2, %1"
               : "=a"(old), "+m"(*memory)
               : "r"(replacement), "0"(expected)
               : "memory");
  return old;
}

int main(void) {
  unsigned char bytes[3] = {91, 129, 93};
  if (cmpxchg_byte(&bytes[1], 129, 237) != 129 || bytes[1] != 237)
    return 1;
  if (cmpxchg_byte(&bytes[1], 129, 42) != 237 || bytes[1] != 237 ||
      bytes[0] != 91 || bytes[2] != 93)
    return 2;
  short words[3] = {1701, -1, 1703};
  if (cmpxchg_word(&words[1], -1, -32768) != -1 || words[1] != -32768)
    return 3;
  if (cmpxchg_word(&words[1], -1, 42) != -32768 || words[1] != -32768 ||
      words[0] != 1701 || words[2] != 1703)
    return 4;
  return 0;
}
