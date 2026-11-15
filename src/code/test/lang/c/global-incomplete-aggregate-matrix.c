/*
 * Fixture: infer the outer extent of a designated aggregate matrix.
 */
struct aggregate_matrix_item {
  unsigned id;
  unsigned devid;
};

static const struct aggregate_matrix_item items[][3] = {
    [0] = {{.id = 4, .devid = 0xa0}, {.id = 5, .devid = 2}, {}},
    [2] = {{.id = 32, .devid = 0xa0}, {}, {}},
};

int main(void) {
  return items[0][0].id != 4 || items[0][1].devid != 2 || items[1][0].id != 0 ||
         items[2][0].id != 32;
}
