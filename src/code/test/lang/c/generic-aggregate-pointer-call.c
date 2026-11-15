/*
 * Fixture: use _Generic to select a call for an aggregate pointer type.
 */
struct dentry {
  int value;
};
struct file_operations {
  int kind;
};
struct short_fops {
  int kind;
};

static struct dentry result;

struct dentry *full(const char *name, unsigned short mode,
                    struct dentry *parent, void *data, const void *aux,
                    const struct file_operations *fops) {
  (void)name;
  (void)mode;
  (void)parent;
  (void)data;
  (void)aux;
  return fops ? &result : (struct dentry *)0;
}

struct dentry *short_form(const char *name, unsigned short mode,
                          struct dentry *parent, void *data, const void *aux,
                          const struct short_fops *fops) {
  (void)name;
  (void)mode;
  (void)parent;
  (void)data;
  (void)aux;
  return fops ? &result : (struct dentry *)0;
}

extern typeof(full) full;

struct dentry *dispatch(const char *name, unsigned short mode,
                        struct dentry *parent, void *data,
                        const struct file_operations *fops) {
  struct dentry *de = _Generic(fops,
      const struct file_operations *: full,
      const struct short_fops *: short_form,
      struct file_operations *: full,
      struct short_fops *: short_form)(name, mode, parent, data, (void *)0,
                                       fops);
  return de;
}

int main(void) {
  struct file_operations fops = {1};
  return dispatch("test", 0, (struct dentry *)0, (void *)0, &fops) == &result
             ? 0
             : 1;
}
