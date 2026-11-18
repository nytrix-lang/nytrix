/*
 * C front-end regression test: linux rep movsl tail.
 */
typedef __SIZE_TYPE__ size_t;

static int copy_tail(void *to, const void *from, size_t n) {
  unsigned long d0, d1, d2;
  asm volatile("rep movsl\n\t"
               "testb $2,%b4\n\t"
               "je 1f\n\t"
               "movsw\n"
               "1:\ttestb $1,%b4\n\t"
               "je 2f\n\t"
               "movsb\n"
               "2:"
               : "=&c"(d0), "=&D"(d1), "=&S"(d2)
               : "0"(n / 4), "q"(n), "1"((long)to), "2"((long)from)
               : "memory");
  return d0 == 0 && d1 == (unsigned long)to + n &&
         d2 == (unsigned long)from + n;
}

int main(void) {
  unsigned char source[48];
  unsigned char destination[48];
  for (size_t i = 0; i < sizeof(source); ++i)
    source[i] = (unsigned char)(i * 13 + 7);
  for (size_t length = 0; length <= 17; ++length) {
    for (size_t offset = 0; offset < 4; ++offset) {
      for (size_t i = 0; i < sizeof(destination); ++i)
        destination[i] = 0xa5;
      if (!copy_tail(destination + offset, source + 3, length))
        return 1;
      for (size_t i = 0; i < sizeof(destination); ++i) {
        unsigned char expected =
            i >= offset && i < offset + length ? source[3 + i - offset] : 0xa5;
        if (destination[i] != expected)
          return 2;
      }
    }
  }
  return 0;
}
