/*
 * C23 fixture for type inference in a statement-expression min macro.
 */
#define min(left, right)                                             \
  ({                                                                 \
    auto inferred_left = (left);                                     \
    auto inferred_right = (right);                                   \
    inferred_left < inferred_right ? inferred_left : inferred_right; \
  })

int main(void) {
  unsigned long large = 0x123456789abcdefUL;
  auto inferred_large = large;
  auto inferred_negative = -1;
  auto pointer = &inferred_large;

  *pointer = min(inferred_large, large + 1);
  return inferred_large == large && inferred_negative < 0 ? 0 : 1;
}
