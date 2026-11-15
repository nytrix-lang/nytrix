/*
 * The inner zero case is impossible only on the guarded path.
 */
extern void unreachable_state(void);
static __attribute__((noinline)) int guarded(int value) {
  if (value != 0) {
    switch (value) {
    case 0:
      unreachable_state();
      return 99;
    case 1:
      return 11;
    default:
      return 17;
    }
  }
  return 23;
}
static __attribute__((noinline)) int joined(int choose, int value) {
  int result = 0;
  if (choose && value != 0)
    result = 7;
  if (value == 0)
    return result + 31;
  return result + 37;
}
static __attribute__((noinline)) int changed(int *value) {
  if (*value != 0) {
    *value = 0;
    if (*value == 0)
      return 41;
  }
  return 43;
}
int main(void) {
  if (guarded(0) != 23 || guarded(1) != 11 || guarded(-1) != 17)
    return 1;
  if (joined(0, 0) != 31 || joined(1, 0) != 31 || joined(0, 1) != 37 ||
      joined(1, 1) != 44)
    return 2;
  int value = 3;
  if (changed(&value) != 41 || value != 0 || changed(&value) != 43)
    return 3;
  return 0;
}
