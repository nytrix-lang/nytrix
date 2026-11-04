struct entry {
  int key;
  int value;
};

struct table {
  struct entry entries[2];
};

static struct table lookup = {
    .entries =
        {
            (struct entry){.key = 3, .value = 11},
            (struct entry){.key = 7, .value = 29},
        },
};

int main(void) {
  return lookup.entries[0].key != 3 || lookup.entries[0].value != 11 ||
         lookup.entries[1].key != 7 || lookup.entries[1].value != 29;
}
