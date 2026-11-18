/*
 * C front-end regression test: linux local static function pointer array.
 */
static int add_one(int value) { return value + 1; }
static int add_two(int value) { return value + 2; }

int main(void) {
  static int (*dispatch[])(int) = {add_one, add_two};
  return dispatch[0](20) != 21 || dispatch[1](20) != 22;
}
