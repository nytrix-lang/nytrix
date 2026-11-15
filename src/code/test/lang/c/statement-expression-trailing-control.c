/*
 * C parser fixture for statement expressions ending in loop control flow.
 */
struct walk {
  unsigned long value;
};

#define SET_FROM_WALK(result, input)     \
  ({                                     \
    unsigned int remaining = (input);    \
    unsigned int offset = 0;             \
    if (remaining != 0) {                \
      struct walk destination, source;   \
      source.value = remaining + offset; \
      destination.value = source.value;  \
      *(result) = destination.value;     \
    }                                    \
  })

int main(void) {
  unsigned long result = 0;
  SET_FROM_WALK(&result, 7);
  return result == 7 ? 0 : 1;
}
