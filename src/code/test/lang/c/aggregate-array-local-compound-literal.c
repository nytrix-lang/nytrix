/*
 * Fixture: initialize a local aggregate array from a compound literal.
 */
struct aggregate_local_item {
  const char *name;
  unsigned value;
};

int main(void) {
  struct aggregate_local_item items[] = {
      (struct aggregate_local_item){.name = "first", .value = 7},
      {},
  };
  return items[0].value != 7 || items[0].name[0] != 'f' || items[1].name != 0 ||
         items[1].value != 0;
}
