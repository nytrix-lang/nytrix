struct entry {
  unsigned value;
};
static const struct entry *const entries[];
static unsigned char bad_chars[] = "*?"
                                   "<>|\"";
extern signed char nyc_high_byte[];
signed char nyc_high_byte[] = "\xff";
const unsigned char nyc_empty_array[] = {};
const char *nyc_empty_names[0] = {};
const struct entry *nyc_empty_entries[0] = {};
static const struct entry item = {23};
static const struct entry *const entries[3] = {&item};

static unsigned padding_sum(void) {
  static const unsigned char padding[16];
  unsigned sum = 0;
  for (unsigned i = 0; i < sizeof(padding); ++i)
    sum += padding[i];
  return sum;
}

int main(void) {
  return sizeof(bad_chars) != 7 || bad_chars[5] != '"' || bad_chars[6] ||
         sizeof(nyc_high_byte) != 2 || nyc_high_byte[0] != -1 ||
         nyc_high_byte[1] || sizeof(nyc_empty_array) != 0 ||
         sizeof(nyc_empty_names) != 0 || sizeof(nyc_empty_entries) != 0 ||
         sizeof(entries) != 3 * sizeof(void *) || entries[0]->value != 23 ||
         entries[1] != 0 || entries[2] != 0 || padding_sum() != 0;
}
