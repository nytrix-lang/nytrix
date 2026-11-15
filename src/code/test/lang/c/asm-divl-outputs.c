typedef unsigned int u32;

struct answer {
  u32 before;
  u32 quotient;
  u32 remainder;
  u32 after;
};

__attribute__((noinline)) void divide_words(u32 low, u32 high, u32 divisor,
                                            struct answer *out) {
  __asm__("divl %2"
          : "=a"(out->quotient), "=d"(out->remainder)
          : "rm"(divisor), "0"(low), "1"(high));
}

int main(void) {
  static const u32 cases[5][5] = {
      {17, 0, 5, 3, 2},
      {0, 1, 3, 0x55555555u, 1},
      {0xffffffffu, 0x7fffffffu, 0x80000000u, 0xffffffffu, 0x7fffffffu},
      {1, 0x80000000u, 0xffffffffu, 0x80000000u, 0x80000001u},
      {0xffffffffu, 0xfffffffeu, 0xffffffffu, 0xffffffffu, 0xfffffffeu}};
  for (u32 i = 0; i < 5; ++i) {
    struct answer out = {0xa5a55a5au, 0, 0, 0x12345678u};
    divide_words(cases[i][0], cases[i][1], cases[i][2], &out);
    if (out.quotient != cases[i][3] || out.remainder != cases[i][4] ||
        out.before != 0xa5a55a5au || out.after != 0x12345678u)
      return (int)i + 1;
  }
  return 0;
}
