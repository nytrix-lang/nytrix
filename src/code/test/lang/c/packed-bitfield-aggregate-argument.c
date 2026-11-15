/*
 * C parser fixture for SysV by-value classification of packed bitfields.
 */
struct enhancement_bits {
  unsigned int bit0 : 1;
  unsigned int bit1 : 1;
  unsigned int bit2 : 1;
  unsigned int bit3 : 1;
  unsigned int bit4 : 1;
  unsigned int bit5 : 1;
  unsigned int bit6 : 1;
  unsigned int bit7 : 1;
  unsigned int bit8 : 1;
  unsigned int bit9 : 1;
  unsigned int bit10 : 1;
  unsigned int bit11 : 1;
  unsigned int bit12 : 1;
  unsigned int bit13 : 1;
  unsigned int bit14 : 1;
  unsigned int bit15 : 1;
} __attribute__((packed));

static unsigned int enabled(struct enhancement_bits value) {
  return value.bit0 + value.bit15;
}

int main(void) {
  struct enhancement_bits value = {.bit0 = 1, .bit15 = 1};
  return sizeof(value) == 2 && enabled(value) == 2 ? 0 : 1;
}
