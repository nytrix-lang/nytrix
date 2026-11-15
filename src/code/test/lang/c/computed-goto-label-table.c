/*
 * Runtime fixture for computed goto dispatch through a label table.
 */
static int dispatch(unsigned index, const unsigned char **target) {
  static void *const targets[] = {&&first, &&second};
  *target = targets[index];
  goto *targets[index];
  return -1;
first:
  return 11;
second:
  return 22;
}

static int backward(const unsigned char **target) {
  goto initialize;
destination:
  return 33;
initialize:
  *target = &&destination;
  goto **target;
}

static int backward_static(const unsigned char **target) {
  goto initialize;
destination:
  return 44;
initialize:
  {
    static void *const targets[] = {&&destination};
    *target = targets[0];
    goto *targets[0];
  }
}

static int valid_target(const unsigned char *target) {
#ifdef EXPECT_IBT
  return target[0] == 0xf3 && target[1] == 0x0f && target[2] == 0x1e &&
         target[3] == 0xfa;
#else
  (void)target;
  return 1;
#endif
}

int main(void) {
  const unsigned char *target = 0;
  if (dispatch(0, &target) != 11 || !valid_target(target))
    return 1;
  if (dispatch(1, &target) != 22 || !valid_target(target))
    return 2;
  if (backward(&target) != 33 || !valid_target(target))
    return 3;
  if (backward_static(&target) != 44 || !valid_target(target))
    return 4;
  return 0;
}
