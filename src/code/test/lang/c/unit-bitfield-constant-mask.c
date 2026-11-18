/*
 * C front-end regression test: unit bitfield constant mask.
 */
extern void __bad_mask(void);

static inline __attribute__((always_inline)) unsigned long
field_multiplier(unsigned long field) {
  if ((field | (field - 1)) & ((field | (field - 1)) + 1))
    __bad_mask();
  return field & -field;
}

int main(int argc, char **argv) {
  unsigned long field = (~0UL >> 56) & ~31UL;
  volatile int sink = 0;
  if (argc > 1)
    sink = argc;
  (void)sink;
  (void)argv;
  return field_multiplier(field) != 0x20;
}
