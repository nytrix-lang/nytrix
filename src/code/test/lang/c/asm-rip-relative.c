extern int image_anchor;

static __attribute__((always_inline)) inline void *relative(void *address) {
  asm("leaq %c1(%%rip), %0" : "=r"(address) : "i"(address));
  return address;
}

int main(void) {
  int *anchor = relative(&image_anchor);
  if (*anchor != 41)
    return 1;
  *anchor += 17;
  return *anchor != 58;
}
