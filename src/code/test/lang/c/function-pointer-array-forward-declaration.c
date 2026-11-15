/*
 * C parser fixture for forward declarations of function-pointer arrays.
 */
static int (*callbacks[])(int);

static int add_one(int value) { return value + 1; }
static int subtract_one(int value) { return value - 1; }

static int (*callbacks[])(int) = {add_one};

typedef int (*nyc_callback_t)(int);

static int call_local_callback(unsigned index, int value) {
  static const nyc_callback_t callbacks[] = {add_one, subtract_one};
  if (callbacks[index])
    return callbacks[index](value);
  return -1;
}

int main(void) {
  return callbacks[0](41) == 42 && call_local_callback(1, 43) == 42 ? 0 : 1;
}
