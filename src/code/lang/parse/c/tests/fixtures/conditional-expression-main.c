/*
 * Runtime harness for conditional-expression and unsigned-comparison fixtures.
 */
long nyr_feature_enabled(void);
long nyr_unsigned_condition(void);
long nyr_compare_select(unsigned char, long, long);
int main(void) {
  return nyr_feature_enabled() != 1 || nyr_unsigned_condition() != 1 ||
         nyr_compare_select(128, 37, 9) != 37 ||
         nyr_compare_select(127, 37, 9) != 9;
}
