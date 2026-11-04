static unsigned int bits;

static unsigned char set_bit_one(void) {
  unsigned char previous;
  asm volatile("lock btsl %[val], %[var]"
               : [var] "+m"(bits), "=@ccc"(previous)
               : [val] "I"(0 + 8)
               : "memory");
  return previous;
}

int main(void) {
  if (set_bit_one() != 0 || bits != 256)
    return 1;
  if (set_bit_one() != 1 || bits != 256)
    return 2;
  return bits != 256;
}
