/*
 * Fixture: take the address of a scalar element in a local multidimensional
 * array.
 */
static void write_byte(unsigned char *value) { *value = 5; }

int main(void) {
  unsigned char values[2][4];
  write_byte(&values[1][0]);
  return values[1][0] == 5 ? 0 : 1;
}
