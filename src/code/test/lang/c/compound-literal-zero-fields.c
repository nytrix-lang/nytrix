struct record {
  unsigned key;
  unsigned limit;
  void (*visit)(void);
};

static void marker(void) {}

static __attribute__((noinline)) int verify_and_change(struct record *record,
                                                       unsigned key) {
  int result = record->key != key || record->limit != 0 || record->visit != 0;
  record->limit = 123;
  record->visit = marker;
  return result;
}

static __attribute__((noinline)) int check_literals(unsigned key) {
  int result = 0;
  for (int i = 0; i < 3; i++) {
    result |= verify_and_change(&(struct record){.key = key + i}, key + i);
    struct record *array = (struct record[2]){{.key = key}, {.key = key + 1}};
    result |= verify_and_change(&array[0], key);
    result |= verify_and_change(&array[1], key + 1);
  }
  return result;
}

int main(void) { return check_literals(37); }
