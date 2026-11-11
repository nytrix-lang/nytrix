/*
 * Fixture: initialize global aggregate arrays with compound literals.
 */
struct hook_entry {
  int key;
  int active;
};

struct hook_table {
  struct hook_entry entries[2];
};

static struct hook_table hooks = {
    .entries =
        {
            (struct hook_entry){.key = 3, .active = 1},
            (struct hook_entry){.key = 5, .active = 1},
        },
};

int main(void) {
  return hooks.entries[0].key != 3 || hooks.entries[1].key != 5 ||
         !hooks.entries[0].active || !hooks.entries[1].active;
}
