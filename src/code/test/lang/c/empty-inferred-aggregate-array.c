/*
 * Fixture: infer aggregate-array initialization when elements are empty.
 */
struct entry {
  int value;
};

static const struct entry empty[] = {};

int main(void) { return sizeof(empty) != 0; }
