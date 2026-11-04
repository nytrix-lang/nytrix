struct nyc_hook_key {
  unsigned long value;
};

struct nyc_hook_key nyc_hook_keys[2][3];

static struct nyc_hook_key *nyc_hook_key_at(unsigned int row,
                                            unsigned int column) {
  return &nyc_hook_keys[row][column];
}

int main(void) { return nyc_hook_key_at(1, 2) != &nyc_hook_keys[1][2]; }
