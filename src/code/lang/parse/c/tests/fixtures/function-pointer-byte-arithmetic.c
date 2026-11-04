static int target(void) { return 1; }

int main(void) {
  int (*function)(void) = target;
  unsigned long original = (unsigned long)function;
  unsigned long advanced = (unsigned long)(function + 1);
  return advanced == original + 1 ? 0 : 1;
}
