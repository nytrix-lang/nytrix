#include <stdarg.h>

extern void unsupported_word_size(void);
extern void unavailable_feature(void);

static inline int feature_enabled(void) { return 0; }

int first_feature_user(void) {
  if (feature_enabled())
    unavailable_feature();
  return 20;
}

int second_feature_user(void) {
  if (feature_enabled())
    unavailable_feature();
  return 22;
}

static int consume(int fixed, ...) {
  va_list args;
  va_start(args, fixed);
  int value = va_arg(args, int);
  switch (sizeof(value)) {
  case 1:
  case 2:
  case 4:
  case 8:
    value += fixed;
    break;
  default:
    unsupported_word_size();
  }
  va_end(args);
  return value;
}

int main(void) {
  return consume(first_feature_user(), second_feature_user()) != 42;
}
