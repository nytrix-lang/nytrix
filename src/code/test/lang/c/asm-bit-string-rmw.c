/*
 * C front-end regression test: asm bit string rmw.
 */
typedef unsigned int u32;
typedef unsigned char u8;

__attribute__((noinline)) u8 bit_test(volatile u32 *base, int index) {
  u8 result;
  __asm__("btl %2,%1" : "=@ccc"(result) : "m"(*base), "Ir"(index));
  return result;
}

__attribute__((noinline)) void bit_set(volatile u32 *base, int index) {
  __asm__ volatile("btsl %1,%0" : "+m"(*base) : "Ir"(index));
}
__attribute__((noinline)) void bit_clear(volatile u32 *base, int index) {
  __asm__ volatile("btrl %1,%0" : "+m"(*base) : "Ir"(index));
}
__attribute__((noinline)) void bit_toggle(volatile u32 *base, int index) {
  __asm__ volatile("btcl %1,%0" : "+m"(*base) : "Ir"(index));
}
__attribute__((noinline)) void bit_set_locked(volatile u32 *base, int index) {
  __asm__ volatile("lock btsl %1,%0" : "+m"(*base) : "Ir"(index) : "memory");
}

int main(void) {
  volatile u32 words[5] = {0x11223344, 0, 0, 0, 0x55667788};
  volatile u32 *base = &words[2];
  bit_set(base, -1);
  bit_set(base, 33);
  if (words[1] != 0x80000000u || words[2] != 0 || words[3] != 2)
    return 1;
  if (!bit_test(base, -1) || !bit_test(base, 33) || bit_test(base, 0))
    return 3;
  bit_toggle(base, -1);
  bit_clear(base, 33);
  bit_set(base, 0);
  bit_set_locked(base, 63);
  if (words[0] != 0x11223344 || words[1] != 0 || words[2] != 1 ||
      words[3] != 0x80000000u || words[4] != 0x55667788)
    return 2;
  return 0;
}
