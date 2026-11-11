/*
 * Fixture: copy pointer values into selected elements of a pointer array.
 */
static int first;
static int second;

static void copy_pointer(void **items) { items[0] = items[1]; }
static void store_const_pointer(const void *items[], const void *value) {
  items[0] = value;
}

int main(void) {
  void *items[2];
  const void *const_items[1];
  items[0] = &first;
  items[1] = &second;
  copy_pointer(items);
  store_const_pointer(const_items, &first);
  return items[0] != &second || const_items[0] != &first;
}
