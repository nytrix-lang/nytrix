/*
 * Runtime fixture checking the requested alignment of a local byte array.
 */
int main(void) {
  unsigned char bytes[24] __attribute__((aligned(8)));
  return ((unsigned long)bytes & 7) != 0;
}
