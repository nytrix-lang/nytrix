/*
 * C preprocessor fixture for conditional directives inside macro arguments.
 */
#define CHECK(...) ((void)(__VA_ARGS__))

int main(void) {
#if 1
  CHECK(
#else
  CHECK(
#endif
      0);
  return 0;
}
