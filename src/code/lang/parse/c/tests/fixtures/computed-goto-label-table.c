static int dispatch(unsigned index) {
  static void *const targets[] = {&&first, &&second};
  goto *targets[index];
  return -1;
first:
  return 11;
second:
  return 22;
}

int main(void) { return dispatch(0) == 11 && dispatch(1) == 22 ? 0 : 1; }
