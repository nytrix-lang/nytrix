static int typeof_call_target(int value) { return value * 3; }
static int typeof_call(int value) {
  return ((typeof(typeof_call_target) *)(typeof_call_target))(value);
}
int main(void) { return typeof_call(14) != 42; }
