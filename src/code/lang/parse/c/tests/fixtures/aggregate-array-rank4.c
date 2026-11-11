/*
 * Fixture: index a four-dimensional aggregate array.
 */
struct grid_owner {
  unsigned char cells[2][2][2][2];
};

static struct grid_owner grid;

int main(void) {
  struct grid_owner *owner = &grid;
  return owner->cells[1][0][1][1] != 0;
}
