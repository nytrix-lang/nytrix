/*
 * C parser fixture for function pointers returning aggregate values.
 */
struct test_inode {
  int value;
};
struct test_dentry {
  int value;
};

struct test_dentry *lookup(struct test_inode *directory,
                           struct test_dentry *entry, unsigned int flags) {
  (void)directory;
  (void)flags;
  return entry;
}
extern typeof(lookup) lookup;

struct test_inode_operations {
  struct test_dentry *(*lookup_fn)(struct test_inode *, struct test_dentry *,
                                   unsigned int);
};

const struct test_inode_operations operations = {.lookup_fn = lookup};

int main(void) {
  struct test_inode directory = {0};
  struct test_dentry entry = {0};
  return operations.lookup_fn(&directory, &entry, 0) == &entry ? 0 : 1;
}
