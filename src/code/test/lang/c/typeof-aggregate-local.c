/*
 * C frontend test fixture: typeof aggregate local.
 */
typedef struct {
  unsigned long bits[1];
} mask;

extern mask type_example;

struct record {
  int count;
  unsigned char mode;
  mask nodes;
};

static struct record records[4];

#define SINGLE_NODE(node)                                                      \
  ({                                                                           \
    __typeof__(type_example) m;                                                \
    if (sizeof(m) == sizeof(unsigned long))                                    \
      m.bits[0] = 1UL << (node);                                               \
    m;                                                                         \
  })

static __attribute__((noinline)) void set_record(unsigned node) {
  records[node] =
      (struct record){.count = 11, .mode = 7, .nodes = SINGLE_NODE(node)};
}

int main(void) {
  set_record(0);
  set_record(2);
  return records[0].nodes.bits[0] != 1 || records[2].nodes.bits[0] != 4 ||
         records[2].count != 11 || records[2].mode != 7;
}
