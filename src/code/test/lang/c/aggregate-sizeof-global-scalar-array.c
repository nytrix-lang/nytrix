/*
 * C parser fixture for sizeof and initialization of global scalar arrays.
 */
static const unsigned char prefix[] = {1, 2, 3};

struct nyc_prefix_entry {
  const unsigned char *data;
  unsigned long size;
};

static const struct nyc_prefix_entry entry = {prefix, sizeof(prefix)};

int main(void) { return entry.size == 3 && entry.data[2] == 3 ? 0 : 1; }
