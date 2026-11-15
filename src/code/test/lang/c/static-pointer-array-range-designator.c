/*
 * Fixture: initialize each static pointer-array element with a GNU range
 * designator.
 */
int main(void) {
  static int target;
  static int *const pointers[4] = {[0 ... 3] = &target};
  return pointers[0] == &target && pointers[1] == &target &&
                 pointers[2] == &target && pointers[3] == &target
             ? 0
             : 1;
}
