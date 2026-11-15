/*
 * C parser fixture for typeof applied to a conditional builtin expression.
 */
int main(void) {
  unsigned long value = 8;
  __typeof__(__builtin_constant_p(value) ? __builtin_ffs((int)value)
                                         : 0) result =
      __builtin_ffs((int)value);
  return sizeof(result) != sizeof(int) || result != 4;
}
