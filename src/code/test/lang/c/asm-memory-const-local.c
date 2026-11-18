/*
 * C front-end regression test: asm memory const local.
 */
__attribute__((noinline)) unsigned long read_const_input(unsigned long input) {
  const unsigned long value = input ^ 0xa5a55a5ac3c33c3cul;
  unsigned long observed = 0;
  __asm__ volatile("movq %1,%0" : "=r"(observed) : "m"(value));
  return observed;
}

int main(void) {
  if (read_const_input(0) != 0xa5a55a5ac3c33c3cul)
    return 1;
  if (read_const_input(0xfedcba9876543210ul) !=
      (0xfedcba9876543210ul ^ 0xa5a55a5ac3c33c3cul))
    return 2;
  if (read_const_input(~0ul) != ~0xa5a55a5ac3c33c3cul)
    return 3;
  return 0;
}
