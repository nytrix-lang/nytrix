/*
 * Unsized local arrays infer their extent from designated indexes, not count.
 */
int main(void) {
  unsigned char values[] = {[0] = 3, [8] = 17, [18] = 29};
  return sizeof(values) != 19 || values[0] != 3 || values[8] != 17 ||
         values[18] != 29;
}
