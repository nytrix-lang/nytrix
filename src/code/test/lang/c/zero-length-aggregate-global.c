/*
 * C parser fixture for zero-length aggregates declared at global scope.
 */
struct range {
  unsigned long start;
  unsigned long end;
};

struct range zero_ranges[0];

int main(void) { return 0; }
