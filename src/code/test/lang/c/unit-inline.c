/*
 * Fixture: inline a static helper into a standalone unit entry point.
 */
static inline __attribute__((always_inline)) int
nyc_unit_inline_add(int value) {
  return value + 7;
}
extern void nyc_unit_inline_forbidden_call(void)
    __attribute__((error("always_inline specialization is required")));
static inline __attribute__((always_inline)) int
nyc_unit_inline_guard(int value) {
  if (value == 0)
    nyc_unit_inline_forbidden_call();
  return value;
}
static inline _Bool nyc_unit_inline_false(void) { return 0; }
static __attribute__((noinline)) void nyc_unit_inline_barrier(void) {}
extern int nyc_unit_inline_unreachable(void);
int main(void) {
  int result = nyc_unit_inline_add(35) != 42;
  result |= nyc_unit_inline_guard(1) != 1;
  nyc_unit_inline_barrier();
  if (nyc_unit_inline_false() && nyc_unit_inline_unreachable())
    result = 1;
  return result;
}
