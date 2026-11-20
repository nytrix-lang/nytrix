/*
 * C frontend test fixture: local pointer to array stride.
 */
typedef struct {
  unsigned long bits;
} entry;

static entry pages[2][512];

static __attribute__((noinline)) void populate(void *storage, unsigned row) {
  entry(*tables)[512] = storage;
  unsigned long *first = &tables[row]->bits;
  *first = 0x1234;
  tables[row][511].bits = 0x9876;
}

int main(void) {
  populate(pages, 1);
  return pages[0][1].bits != 0 || pages[1][0].bits != 0x1234 ||
         pages[1][511].bits != 0x9876;
}
