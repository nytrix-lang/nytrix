enum { ITEM_FIRST = 0, ITEM_LAST = 3 };
union deprecated_type {
  unsigned int value;
} __attribute__((deprecated));
struct unused_type { unsigned int value; } __attribute__((unused));
struct volatile_field { __volatile__ unsigned int value; };
_Static_assert(sizeof(struct volatile_field) == sizeof(unsigned int),
               "GNU volatile spelling preserves field layout");
struct item;
extern struct item *redeclared_pointer;
extern struct item *redeclared_pointer;
struct item { int value; };
struct item *redeclared_pointer = 0;
struct pointer_holder { void *value; };
static struct pointer_holder holder = {0};
static int pointer_sentinel(void) { return 0; }
static const struct item items[] = {
  [ITEM_LAST] = {40}, {50}, [ITEM_FIRST] = {10}
};
static int check_local(void) {
  static const struct item local_items[] = {
    [ITEM_LAST] = {40}, {50}, [ITEM_FIRST] = {10}
  };
  struct item runtime_items[3];
  unsigned char runtime_bytes[5];
  return sizeof(runtime_items) != 3 * sizeof(struct item) ||
         sizeof(runtime_bytes) != 5 ||
         sizeof(local_items) / sizeof(local_items[0]) != 5 ||
         local_items[0].value != 10 || local_items[1].value != 0 ||
         local_items[2].value != 0 || local_items[3].value != 40 ||
         local_items[4].value != 50;
}
static int check_tag_shadow(void) {
  struct item { long value; } shadow = {77};
  return sizeof(struct item) != sizeof(long) || shadow.value != 77;
}
int main(void) {
  return redeclared_pointer != 0 || holder.value != 0 ||
         holder.value == pointer_sentinel || check_local() ||
         check_tag_shadow() ||
         sizeof(items) / sizeof(items[0]) != 5 ||
         items[0].value != 10 || items[1].value != 0 ||
         items[2].value != 0 || items[3].value != 40 ||
         items[4].value != 50;
}
