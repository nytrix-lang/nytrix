int nyc_compound_array_a(void) {
  static int *values = (int[]){3, 5};
  return values[1];
}
