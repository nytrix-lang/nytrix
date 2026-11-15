/*
 * C parser fixture for nested const-qualified function-pointer parameters.
 */
static int target(int value) { return value + 1; }

static int invoke(int (*const *callback)(int), int value) {
  return (*callback)(value);
}

int main(void) {
  int (*callback)(int) = target;
  return invoke(&callback, 41) != 42;
}
