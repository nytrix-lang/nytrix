/*
 * Fixture: retain unreachable inline assembly in a guarded control-flow path.
 */
__attribute__((noinline)) static int guarded(int condition) {
  if (condition)
    do {
      ({ __asm__ volatile("9: nop"); });
      do {
        __asm__ volatile("1:\tud2\n\t.pushsection .discard.test, \"aw\"\n"
                         "\t.word %c[flags]\n\t.popsection"
                         :
                         : [flags] "i"(0));
      } while (0);
      __builtin_unreachable();
    } while (0);
  return 42;
}

int main(void) { return guarded(0) != 42; }
