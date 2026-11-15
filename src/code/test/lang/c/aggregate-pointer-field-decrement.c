/*
 * Fixture: decrement a pointer-valued aggregate field.
 */
struct nyc_cursor_item {
  long value;
};

struct nyc_cursor_holder {
  struct nyc_cursor_item *cursor;
};

int main(void) {
  struct nyc_cursor_item items[2];
  struct nyc_cursor_holder holder;
  holder.cursor = &items[1];
  holder.cursor--;
  return holder.cursor != &items[0];
}
