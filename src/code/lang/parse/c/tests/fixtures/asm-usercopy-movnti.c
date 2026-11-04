static void copy_32(const unsigned long *source, unsigned long *dest) {
  __asm__("movq    (%0), %%r8\n"
          "movq   8(%0), %%r9\n"
          "movq  16(%0), %%r10\n"
          "movq  24(%0), %%r11\n"
          "movnti  %%r8,   (%1)\n"
          "movnti  %%r9,  8(%1)\n"
          "movnti %%r10, 16(%1)\n"
          "movnti %%r11, 24(%1)\n"
          :
          : "r"(source), "r"(dest)
          : "memory", "r8", "r9", "r10", "r11");
}

static void copy_8(const unsigned long *source, unsigned long *dest) {
  __asm__ volatile("movq    (%0), %%r8\n"
                   "movnti  %%r8,   (%1)\n"
                   :
                   : "r"(source), "r"(dest)
                   : "memory", "r8");
}

static void copy_4(const unsigned int *source, unsigned int *dest) {
  __asm__ volatile("movl    (%0), %%r8d\n"
                   "movnti  %%r8d,   (%1)\n"
                   :
                   : "r"(source), "r"(dest)
                   : "memory", "r8");
}

int main(void) {
  unsigned long source_32[4] = {0x1020304050607080UL, 0x1122334455667788UL,
                                0x8877665544332211UL, 0xaabbccddeeff0123UL};
  unsigned long dest_32[4] = {0};
  unsigned long source_8 = 0xfedcba9876543210UL, dest_8 = 0;
  unsigned int source_4 = 0x89abcdefU, dest_4 = 0;
  copy_32(source_32, dest_32);
  copy_8(&source_8, &dest_8);
  copy_4(&source_4, &dest_4);
  __sync_synchronize();
  return dest_32[0] != source_32[0] || dest_32[1] != source_32[1] ||
         dest_32[2] != source_32[2] || dest_32[3] != source_32[3] ||
         dest_8 != source_8 || dest_4 != source_4;
}
