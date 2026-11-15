/*
 * Fixture: initialize static pointer tables with designated indices.
 */
static const char *nyc_pointer_labels[] = {"zero", "one"};
static const char **nyc_pointer_table[4] = {
    [2] = nyc_pointer_labels,
};

int main(void) {
  return nyc_pointer_table[0] != 0 || nyc_pointer_table[1] != 0 ||
         nyc_pointer_table[2] != nyc_pointer_labels ||
         nyc_pointer_table[3] != 0;
}
