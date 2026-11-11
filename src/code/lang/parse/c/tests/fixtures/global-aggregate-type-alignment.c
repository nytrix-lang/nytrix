/*
 * Fixture: preserve explicit alignment for a global aggregate type.
 */
struct nyc_aligned_key {
  unsigned long key[2];
};

static struct nyc_aligned_key __attribute__((aligned(16))) nyc_keys[2];

int main(void) {
  return ((unsigned long)&nyc_keys[0] & 15) != 0 ||
         ((unsigned long)&nyc_keys[1] & 15) != 0;
}
