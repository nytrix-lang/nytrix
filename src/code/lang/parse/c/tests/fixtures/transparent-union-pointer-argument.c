/*
 * C parser fixture for pointer arguments passed through a transparent union.
 */
typedef union {
  int *integer;
  void *generic;
} pointer_argument __attribute__((__transparent_union__));

static int *take_pointer(pointer_argument argument) { return argument.integer; }

int main(void) {
  int value;
  return take_pointer(&value) != &value;
}
