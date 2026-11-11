/*
 * Fixture: evaluate sizeof and typeof with pointers to incomplete aggregates.
 */
struct incomplete;

static void *allocate(unsigned long size) {
  (void)size;
  return (void *)0;
}

#define allocate_objects(P, COUNT)                                     \
  ({                                                                   \
    const unsigned long object_size = sizeof(__typeof__(P)) * (COUNT); \
    (__typeof__(P) *)allocate(object_size);                            \
  })

int main(void) {
  struct incomplete **items = allocate_objects(*items, 3);
  return sizeof(__typeof__(*items)) == sizeof(void *) && !items ? 0 : 1;
}
