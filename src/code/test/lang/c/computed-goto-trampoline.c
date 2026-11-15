/*
 * Local static label tables keep their target blocks addressable even when
 * the optimizer threads an entry label's unconditional branch to a successor.
 */
static int dispatch(unsigned index) {
  static void *targets[] = {&&trampoline, &&destination};
  goto *targets[index];
  return -1;
trampoline:
  goto destination;
destination:
  return 42;
}

int main(void) { return dispatch(0) == 42 && dispatch(1) == 42 ? 0 : 1; }
