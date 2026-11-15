/*
 * Fixture: query the size and layout of an anonymous aggregate member.
 */
struct filename_head {
  const char *name;
  int refcnt;
  void *audit_name;
};

struct filename {
  struct filename_head;
  const char iname[192 - sizeof(struct filename_head)];
};

_Static_assert(__builtin_offsetof(struct filename, iname) % sizeof(long) == 0,
               "embedded name must be long-aligned");
_Static_assert(sizeof(struct filename) % 64 == 0,
               "filename storage must be cacheline-sized");

int main(void) {
  struct filename filename;
  return sizeof(filename) != 192 ||
         __builtin_offsetof(struct filename, iname) != 24;
}
