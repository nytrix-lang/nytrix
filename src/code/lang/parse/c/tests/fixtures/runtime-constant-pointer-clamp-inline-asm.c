#define nyc_runtime_user_ptr_max()                                             \
  ({                                                                           \
    unsigned long result;                                                      \
    asm("mov %1,%0\n1:\n"                                                      \
        ".pushsection runtime_ptr_nyc_fixture_user_ptr_max,\"a\"\n\t"          \
        ".long 1b - %c2 - .\n"                                                 \
        ".popsection"                                                          \
        : "=r"(result)                                                         \
        : "i"(0x0123456789abcdefull), "i"(sizeof(long)));                      \
    result;                                                                    \
  })

static unsigned long nyc_clamp_user_pointer(unsigned long pointer) {
  unsigned long result;
  asm("cmp %1,%0\n\tcmova %1,%0"
      : "=r"(result)
      : "r"(nyc_runtime_user_ptr_max()), "0"(pointer));
  return result;
}

int main(void) {
  unsigned long maximum = nyc_runtime_user_ptr_max();
  return nyc_clamp_user_pointer(maximum - 1) != maximum - 1 ||
         nyc_clamp_user_pointer(maximum) != maximum ||
         nyc_clamp_user_pointer(maximum + 1) != maximum;
}
