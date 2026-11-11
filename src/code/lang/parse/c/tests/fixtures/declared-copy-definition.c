/* A visible user definition must retain its side effects. */
static unsigned calls;

void *memcpy(void *destination, const void *source, unsigned long count) {
  unsigned char *dst = destination;
  const unsigned char *src = source;
  ++calls;
  for (unsigned long i = 0; i < count; ++i)
    dst[i] = src[i];
  return destination;
}

int main(void) {
  char result[3] = {0};
  void *returned = memcpy(result, "abc", 3);
  return calls == 1 && returned == result && result[0] == 'a' &&
                 result[1] == 'b' && result[2] == 'c'
             ? 0
             : 1;
}
