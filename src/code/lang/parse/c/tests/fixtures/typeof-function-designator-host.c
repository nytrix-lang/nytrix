struct nyc_typeof_ops { int (*call)(int, int); };
struct nyc_typeof_ops nyc_typeof_ops;
int nyc_typeof_trampoline(int left, int right) { return left + right; }
int nyc_typeof_function_designator_call(int left, int right);
int main(void) {
  return nyc_typeof_function_designator_call(19, 23) != 42;
}
