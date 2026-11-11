/*
 * Fixture: place an unused asm-goto function in a dead-code path.
 */
static inline int nyc_unused_asm_goto(void) {
  asm goto("" : : : : done);
  return 0;
done:
  return 1;
}
int main(void) { return 0; }
