/*
 * Fixture: preserve an incomplete aggregate-array pointer in a struct field.
 */
struct array_item {
  int value;
};

struct array_holder {
  const struct array_item (*items)[];
};

static const struct array_item items[] = {{5}, {7}};
static const struct array_holder holder = {.items = &items};

int main(void) {
  const struct array_item *values = *holder.items;
  return values[0].value != 5 || values[1].value != 7;
}
