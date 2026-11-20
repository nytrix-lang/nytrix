/*
 * C frontend test fixture: bool pointer call.
 */
typedef _Bool flag;

static __attribute__((noinline)) flag *set_flag(flag *out, flag value) {
  flag *result = out;
  *result = value;
  return result;
}

int main(void) {
  flag values[2] = {0, 0};
  flag *first = &values[0];
  flag *second;
  second = &values[1];
  if (set_flag(second, first) != second || values[0] || !values[1])
    return 1;
  if (set_flag(second, (flag *)0) != second || values[1])
    return 2;
  return 0;
}
