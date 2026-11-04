static unsigned long nyc_cmpxchg64_value;
int main(void) {
  unsigned long old = 0, replacement = 1; unsigned char success;
  asm volatile("cmpxchgq %[nval], %%gs:%[var]"
      : "=@ccz"(success), [oval] "+a"(old), [var] "+m"(nyc_cmpxchg64_value)
      : [nval] "r"(replacement) : "memory");
  return !success || old != 0 || nyc_cmpxchg64_value != 1;
}
