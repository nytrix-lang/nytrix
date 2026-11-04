#include <stddef.h>

struct value {
  int number;
};

struct inner_alignment {
  unsigned char tag;
  struct value *__attribute__((aligned(32))) * link;
};

struct outer_alignment {
  unsigned char tag;
  struct value **__attribute__((aligned(32))) link;
};

static int is_tagged(struct value *__attribute__((aligned(1))) * value) {
  return (unsigned long)value & 1;
}

static int read_nonnull(const int *value) __attribute__((nonnull));

static int read_nonnull(const int *value) { return *value; }

int main(void) {
  struct inner_alignment holder = {1, 0};
  int number = 9;
  return offsetof(struct inner_alignment, link) != sizeof(void *) ||
         sizeof(holder) != 2 * sizeof(void *) ||
         offsetof(struct outer_alignment, link) != 32 ||
         sizeof(struct outer_alignment) != 64 || is_tagged(holder.link) ||
         !is_tagged((struct value **)1) || read_nonnull(&number) != 9;
}
