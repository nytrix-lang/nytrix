static int add_one(int value) { return value + 1; }
static int subtract_one(int value) { return value - 1; }

int main(void) {
  static int (*const selected)(int) = 1 ? add_one : subtract_one;
  int (*function)(int) = add_one;
  return _Generic(function, int (*)(int): add_one, default: subtract_one)(41) ==
                     42 &&
                 selected(41) == 42
             ? 0
             : 1;
}
