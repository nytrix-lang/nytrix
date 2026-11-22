/*
 * Verify volatile global array accesses preserve every load and store without
 * redundant-load or dead-store elimination.
 */
extern volatile int words[4];
volatile int words[4];
volatile unsigned short halfwords[4];
volatile int rows[2][3];

int read_after_write(int value) {
  words[2] = value;
  return words[2];
}

void read_twice(void) {
  (void)words[2];
  (void)words[2];
}

unsigned short read_halfword(unsigned short value) {
  halfwords[2] = value;
  return halfwords[2];
}

int read_row(int value) {
  rows[1][2] = value;
  return rows[1][2];
}
