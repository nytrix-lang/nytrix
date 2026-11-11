/*
 * Fixture: call a helper that transfers control through asm-goto.
 */
static inline int nyc_used_asm_goto(void) {
  asm goto("" : : : : done);
  return 0;
done:
  return 1;
}
int main(void) { return nyc_used_asm_goto(); }
