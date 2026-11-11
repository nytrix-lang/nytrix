/*
 * C preprocessor fixture for conditional directives closing macro arguments.
 */
#define ID(value) (value)

int main(void) {
  int first = ID(
#if 1
      3);
#else
      4);
#endif
  int second = ID(
#if 0
      5);
#else
      6);
#endif
  return first != 3 || second != 6;
}
