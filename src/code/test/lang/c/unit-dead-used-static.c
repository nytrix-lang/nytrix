/*
 * Fixture: retain used static data declared in a constant-false C arm.
 */
int nyc_dead_used_static_target;
int nyc_inlined_used_static_target;

static int dead_used_static_anchor(void) {
  if (0) {
    static void *__attribute__((used, section(".discard.addressable"))) anchor =
        &nyc_dead_used_static_target;
    (void)anchor;
  }
  return 0;
}

static inline __attribute__((always_inline)) void
inlined_used_static_anchor(int enabled) {
  if (__builtin_expect(!!enabled, 0)) {
    static void *__attribute__((used, section(".discard.addressable"))) anchor =
        (void *)(unsigned long)&nyc_inlined_used_static_target;
  }
}

int main(void) {
  inlined_used_static_anchor(1);
  return dead_used_static_anchor();
}
