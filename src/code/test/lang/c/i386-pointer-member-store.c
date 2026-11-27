/*
 * Storing a pointer into a struct member must write four bytes on i386; an
 * eight-byte store clobbers the neighboring field.
 */
struct card {
  const char *name;
  int (*set)(void *);
  void *modes;
  int count;
};

void nyc_set_name(struct card *c, const char *name) { c->name = name; }
void nyc_set_modes(struct card *c, void *modes) { c->modes = modes; }
