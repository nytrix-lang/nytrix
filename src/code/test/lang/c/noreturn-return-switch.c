/*
 * Returning a no-return call is valid; following switch cases still need
 * lowering after that terminated arm.
 */
static long c_noreturn_switch_stop(void) __attribute__((noreturn));

static long c_noreturn_switch_stop(void) {
  for (;;) {
  }
}

static long c_noreturn_switch_value(unsigned long value) {
  switch (value) {
  case 0:
    return c_noreturn_switch_stop();
  case 1:
    return 40;
  default:
    return 2;
  }
}

int main(void) {
  return c_noreturn_switch_value(1) != 40 || c_noreturn_switch_value(9) != 2;
}
