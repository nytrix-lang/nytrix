/*
 * Fixture: verify an unused inline function's static pointer table is
 * discarded.
 */
static inline const char *nyc_unused_inline_static_names(unsigned index) {
  static const char *const names[] = {"discard-me-a", "discard-me-b"};
  return names[index];
}

const char *nyc_used_static_names(unsigned index) {
  static const char *const names[] = {"keep-me-a", "keep-me-b"};
  return names[index];
}
