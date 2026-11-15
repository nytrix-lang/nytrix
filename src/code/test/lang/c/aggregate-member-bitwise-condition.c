/*
 * Fixture: parse a member bitwise expression as a statement, not an assignment.
 */
struct aggregate_flags {
  unsigned long bits;
};

static int enabled;
static int disabled;

static void enable(void) { ++enabled; }
static void disable(void) { ++disabled; }

int main(void) {
  struct aggregate_flags flags = {1};
  flags.bits & 1 ? enable() : disable();
  return enabled != 1 || disabled != 0;
}
