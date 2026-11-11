/*
 * Fixture: store through a two-dimensional array of pointers.
 */
struct pointer_table {
  char **entries[2];
};

static char marker[] = "m";

int main(void) {
  char *values[2] = {0};
  struct pointer_table table;

  table.entries[0] = values;
  table.entries[1] = values + 1;
  table.entries[0][1] = marker;
  return values[1] != marker;
}
