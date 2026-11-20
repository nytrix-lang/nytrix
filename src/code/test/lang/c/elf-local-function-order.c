static __attribute__((noinline)) int local_first(int x) { return x + 7; }
int exported_first(int x) { return local_first(x); }

static __attribute__((noinline)) int local_second(int x) { return x * 3; }
int exported_last(int x) { return local_second(x) + exported_first(x); }
int exported_tail(int x) { return local_second(x) - x; }

int main(void) {
  volatile int input = 5;
  return exported_last(input) != 27 || exported_tail(input) != 10;
}
