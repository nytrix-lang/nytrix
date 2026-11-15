/*
 * Regression for kernel-compatible conditional expressions with one void arm.
 */
static void *copy_bytes(void *dst, const void *src, unsigned long size) {
  unsigned char *out = dst;
  const unsigned char *in = src;
  for (unsigned long i = 0; i < size; ++i)
    out[i] = in[i];
  return dst;
}

static void zero_bytes(void *dst, unsigned long size) {
  unsigned char *out = dst;
  for (unsigned long i = 0; i < size; ++i)
    out[i] = 0;
}

static void copy_or_zero(int condition, void *dst, const void *src,
                         unsigned long size) {
  (condition ? copy_bytes(dst, src, size) : zero_bytes(dst, size));
}

int main(void) {
  unsigned char dst[4] = {9, 9, 9, 9};
  const unsigned char src[4] = {1, 2, 3, 4};
  copy_or_zero(1, dst, src, 4);
  for (unsigned long i = 0; i < 4; ++i)
    if (dst[i] != src[i])
      return 1;
  copy_or_zero(0, dst, src, 4);
  for (unsigned long i = 0; i < 4; ++i)
    if (dst[i] != 0)
      return 2;
  return 0;
}
