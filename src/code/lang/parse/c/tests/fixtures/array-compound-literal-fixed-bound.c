/*
 * Fixture: preserve a fixed array compound-literal bound and zero-fill its
 * tail.
 */
int main(void) {
  char *bytes = (char[4]){'A'};
  return bytes[0] != 'A' || bytes[1] != 0 || bytes[3] != 0;
}
