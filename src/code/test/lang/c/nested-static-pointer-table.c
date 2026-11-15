/*
 * Fixture: retain a static pointer table referenced only by another table.
 */
const char *nyc_nested_static_names(unsigned index) {
  static const char *const names[] = {"nested-a", "nested-b"};
  static const char *const *const groups[] = {names};
  return groups[0][index];
}
