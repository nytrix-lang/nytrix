/*
 * Fixture: inline a static helper into a standalone unit entry point.
 */
static inline __attribute__((always_inline)) int
nyc_unit_inline_add(int value) {
  return value + 7;
}
int main(void) { return nyc_unit_inline_add(35) != 42; }
