static const unsigned char hash_prefix_none[] = {};

struct prefix_entry {
  const unsigned char *data;
  unsigned long size;
};

static const struct prefix_entry entry = {hash_prefix_none,
                                          sizeof(hash_prefix_none)};

int main(void) {
  return entry.size == 0 && entry.data == hash_prefix_none ? 0 : 1;
}
