/*
 * C front-end regression test: compound array b.
 */
int nyc_compound_array_b(void) {
  static int *values = (int[]){7, 11};
  return values[1];
}
