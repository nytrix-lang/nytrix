/*
 * Fixture: check inline-function boundaries and unused static declarations.
 */
static int step(int value) { return value * 2 + 3; }

__attribute__((unused)) static int unused_before_compaction(int value) {
  return value + 1;
}

static int triangular(int value) {
  if (value == 0)
    return 0;
  return value + triangular(value - 1);
}

static void *trailing_frame(void) __attribute__((__noinline__));
static void *trailing_frame(void) { return __builtin_return_address(0); }

__attribute__((noinline)) static void *leading_frame(void) {
  return __builtin_return_address(0);
}

int main(void) {
  int (*saved_step)(int) = step;
  if (step(-7) != -11 || step(9) != 21 || saved_step(4) != 11)
    return 1;
  if (triangular(10) != 55)
    return 2;
  void *caller = __builtin_return_address(0);
  void *trailing = trailing_frame();
  void *leading = leading_frame();
  if (!trailing || !leading || trailing == caller || leading == caller)
    return 3;
  return 0;
}
