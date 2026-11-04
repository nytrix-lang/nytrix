struct aggregate_char_matrix {
  unsigned count;
  char rows[2][8];
};

static const struct aggregate_char_matrix global_matrix = {
    .count = 2,
    .rows = {"alpha", "beta"},
};

int main(void) {
  struct aggregate_char_matrix local_matrix = {
      .count = 2,
      .rows = {"gamma", "delta"},
  };
  return global_matrix.rows[0][0] != 'a' || global_matrix.rows[1][0] != 'b' ||
         local_matrix.rows[0][0] != 'g' || local_matrix.rows[1][0] != 'd';
}
