unsigned char nyc_xchg8(unsigned char *address, unsigned char value) {
  __asm__ volatile("xchgb %b0, %1"
                   : "+q"(value), "+m"(*address) : : "memory", "cc");
  return value;
}
signed char nyc_xchg_s8(signed char *address, signed char value) {
  __asm__ volatile("xchgb %b0, %1"
                   : "+q"(value), "+m"(*address) : : "memory", "cc");
  return value;
}
unsigned short nyc_xchg16(unsigned short *address, unsigned short value) {
  __asm__ volatile("xchgw %w0, %1"
                   : "+r"(value), "+m"(*address) : : "memory", "cc");
  return value;
}
signed short nyc_xchg_s16(signed short *address, signed short value) {
  __asm__ volatile("xchgw %w0, %1"
                   : "+r"(value), "+m"(*address) : : "memory", "cc");
  return value;
}
unsigned int nyc_xchg32(unsigned int *address, unsigned int value) {
  __asm__ volatile("xchgl %0, %1"
                   : "+r"(value), "+m"(*address) : : "memory", "cc");
  return value;
}
unsigned long nyc_xchg64(unsigned long *address, unsigned long value) {
  __asm__ volatile("xchgq %q0, %1"
                   : "+r"(value), "+m"(*address) : : "memory", "cc");
  return value;
}
int main(void) {
  unsigned char u8 = 3;
  signed char s8 = -7;
  unsigned short u16 = 5;
  signed short s16 = -9;
  unsigned int u32 = 11;
  unsigned long u64 = 13;
  if (nyc_xchg8(&u8, 17) != 3 || u8 != 17) return 1;
  if (nyc_xchg_s8(&s8, 19) != -7 || s8 != 19) return 2;
  if (nyc_xchg16(&u16, 23) != 5 || u16 != 23) return 3;
  if (nyc_xchg_s16(&s16, 29) != -9 || s16 != 29) return 4;
  if (nyc_xchg32(&u32, 31) != 11 || u32 != 31) return 5;
  if (nyc_xchg64(&u64, 37) != 13 || u64 != 37) return 6;
  return 0;
}
