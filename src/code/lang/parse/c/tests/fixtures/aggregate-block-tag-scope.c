/*
 * C parser fixture for block-scoped aggregate tags and shadowing.
 */
int main(void) {
  unsigned long outer_size = 0;
  unsigned long nested_size = 0;
  unsigned long sibling_size = 0;
  {
    struct reused_tag {
      int value;
    };
    outer_size = sizeof(struct reused_tag);
    {
      struct reused_tag {
        long long first;
        long long second;
      };
      nested_size = sizeof(struct reused_tag);
    }
    if (sizeof(struct reused_tag) != 4)
      return 1;
  }
  {
    struct reused_tag {
      char bytes[16];
    };
    sibling_size = sizeof(struct reused_tag);
  }
  return outer_size == 4 && nested_size == 16 && sibling_size == 16 ? 0 : 2;
}
