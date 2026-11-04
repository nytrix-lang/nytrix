extern inline __attribute__((gnu_inline)) int plus_one(int value) {
  return value + 1;
}

static int call_plus_one(int value) { return plus_one(value); }

int main(void) { return call_plus_one(41) == 42 ? 0 : 1; }
