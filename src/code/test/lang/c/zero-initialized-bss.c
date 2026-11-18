/*
 * Exercise zero-fill storage, explicit placement and relocations.
 */
volatile unsigned char zero_bytes[65536];
volatile unsigned long zero_words[64] = {0};
volatile int explicit_zero __attribute__((section(".keepzero"))) = 0;
volatile int nonzero = 7;
volatile int *relocated = &nonzero;

int main(void) {
  if (zero_bytes[0] || zero_bytes[65535] ||
      zero_words[0] || zero_words[63] || explicit_zero || *relocated != 7)
    return 1;
  zero_bytes[65535] = 19;
  zero_words[63] = 29;
  explicit_zero = 37;
  return zero_bytes[65535] != 19 || zero_words[63] != 29 || explicit_zero != 37;
}
