/*
 * C front-end regression test: asm repeat copy tail.
 */
typedef __SIZE_TYPE__ usize;
#define WORD_BYTES __SIZEOF_POINTER__

struct result {
  usize remaining;
  unsigned char *destination;
  const unsigned char *source;
};

__attribute__((noinline)) void copy_tail(unsigned char *destination,
                                         const unsigned char *source,
                                         usize words, usize tail, int backward,
                                         struct result *out) {
  usize count = 91, end_destination = 92, end_source = 93;
  if (backward)
    __asm__ volatile("std");
#if __SIZEOF_POINTER__ == 8
  __asm__ volatile("rep movsq\n\tmovq %4,%%rcx\n\trep movsb"
                   : "=&c"(count), "=&D"(end_destination), "=&S"(end_source)
                   : "0"(words), "g"(tail), "1"(destination), "2"(source)
                   : "memory");
#else
  __asm__ volatile("rep movsl\n\tmovl %4,%%ecx\n\trep movsb"
                   : "=&c"(count), "=&D"(end_destination), "=&S"(end_source)
                   : "0"(words), "g"(tail), "1"(destination), "2"(source)
                   : "memory");
#endif
  if (backward)
    __asm__ volatile("cld");
  out->remaining = count;
  out->destination = (unsigned char *)end_destination;
  out->source = (const unsigned char *)end_source;
}

int main(void) {
  unsigned char source[64], destination[64];
  struct result out;
  copy_tail(0, 0, 0, 0, 0, &out);
  if (out.remaining || out.destination || out.source)
    return 1;
  for (usize i = 0; i < 64; ++i)
    source[i] = (unsigned char)(i * 7 + 3);
  for (usize length = 0; length < 36; ++length) {
    for (usize i = 0; i < 64; ++i)
      destination[i] = 0xa5;
    copy_tail(destination + 3, source + 5, length / WORD_BYTES,
              length % WORD_BYTES, 0, &out);
    if (out.remaining || out.destination != destination + 3 + length ||
        out.source != source + 5 + length)
      return 2;
    for (usize i = 0; i < 64; ++i) {
      unsigned char expected = i >= 3 && i < 3 + length ? source[i + 2] : 0xa5;
      if (destination[i] != expected)
        return 3;
    }
  }
  for (usize i = 0; i < 64; ++i)
    destination[i] = 0xa5;
  copy_tail(destination + 3, source + 5, 1, 10, 0, &out);
  if (out.remaining || out.destination != destination + 13 + WORD_BYTES ||
      out.source != source + 15 + WORD_BYTES)
    return 4;
  for (usize i = 0; i < 64; ++i) {
    unsigned char expected =
        i >= 3 && i < 13 + WORD_BYTES ? source[i + 2] : 0xa5;
    if (destination[i] != expected)
      return 5;
  }
  for (usize i = 0; i < 64; ++i)
    destination[i] = 0xa5;
  copy_tail(destination + 40, source + 40, 2, 3, 1, &out);
  if (out.remaining || out.destination != destination + 37 - 2 * WORD_BYTES ||
      out.source != source + 37 - 2 * WORD_BYTES)
    return 6;
  for (usize i = 0; i < 64; ++i) {
    int copied = (i >= 40 - WORD_BYTES && i < 40 + WORD_BYTES) ||
                 (i >= 38 - 2 * WORD_BYTES && i <= 40 - 2 * WORD_BYTES);
    unsigned char expected = copied ? source[i] : 0xa5;
    if (destination[i] != expected)
      return 7;
  }
  return 0;
}
