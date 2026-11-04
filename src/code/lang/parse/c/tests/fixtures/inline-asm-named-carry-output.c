static unsigned long bits = 8;
static unsigned char clear_bit(unsigned long bit) {
  unsigned char carry;
  __asm__ volatile("lock btrq %[bit], %[bits]"
                   : [bits] "+m"(bits), "=@ccc"(carry)
                   : [bit] "Ir"(bit) : "memory");
  return carry;
}
int main(void) {
  return clear_bit(3) != 1 || clear_bit(2) != 0 || bits != 0;
}
