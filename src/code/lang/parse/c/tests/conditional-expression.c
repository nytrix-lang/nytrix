/*
 * Preprocessor fixture for evaluating conditional expressions in directives.
 */
#define NYR_FEATURE 1
#if NYR_FEATURE ? (2 * 3) : (1 / 0)
long nyr_feature_enabled(void) { return 1; }
#else
long nyr_feature_enabled(void) { return 0; }
#endif

#if 0xdead000000000000UL > 0
long nyr_unsigned_condition(void) { return 1; }
#else
long nyr_unsigned_condition(void) { return 0; }
#endif

long nyr_compare_select(unsigned char value, long yes, long no) {
  return value == 128 ? yes : no;
}
