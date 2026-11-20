/*
 * C frontend test fixture: aggregate double pointer address.
 */
struct section {
  unsigned long flags;
  unsigned long payload;
};

struct section **sections;

static __attribute__((noinline)) struct section *
global_at(unsigned long root, unsigned long index) {
  return &sections[root][index];
}

static __attribute__((noinline)) unsigned long *
local_payload(struct section **table, unsigned long root, unsigned long index) {
  return &table[root][index].payload;
}

int main(void) {
  struct section a[2] = {{0, 1}, {2, 3}};
  struct section b[3] = {{10, 11}, {20, 21}, {30, 31}};
  struct section *table[2] = {a, b};
  sections = table;
  if (global_at(1, 2) != &b[2] || global_at(1, 1)->flags != 20)
    return 1;
  *local_payload(table, 1, 1) = 99;
  return b[1].payload != 99 || b[0].payload != 11 || b[2].payload != 31;
}
