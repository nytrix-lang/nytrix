struct entry {
  int value;
};

static const struct entry empty[] = {};

int main(void) { return sizeof(empty) != 0; }
