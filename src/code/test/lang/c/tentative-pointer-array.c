/*
 * C frontend test fixture: tentative pointer array.
 */
struct record {
  unsigned long value;
};

extern const struct record *records[];
const struct record *records[2];

int *scalars[];
extern int *scalars[3];

unsigned long witness = 0x1122334455667788UL;
static const struct record entry = {29};
static int payload = 53;

int main(void) {
  if (sizeof(records) != 2 * sizeof(void *) ||
      sizeof(scalars) != 3 * sizeof(void *) || records[0] || records[1] ||
      scalars[0] || scalars[1] || scalars[2])
    return 1;
  records[1] = &entry;
  scalars[2] = &payload;
  return witness != 0x1122334455667788UL || records[0] || scalars[0] ||
         records[1]->value != 29 || scalars[2] != &payload;
}
