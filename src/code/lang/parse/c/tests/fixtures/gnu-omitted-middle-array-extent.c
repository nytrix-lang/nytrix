/*
 * C parser regression for GNU omitted-middle conditionals in array bounds.
 */
struct gnu_array_extent {
  unsigned char jumbo[9000 >> 12 ?: 1];
  unsigned char fallback[0 ?: 1];
};

int main(void) {
  return sizeof(((struct gnu_array_extent *)0)->jumbo) == 2 &&
                 sizeof(((struct gnu_array_extent *)0)->fallback) == 1
             ? 0
             : 1;
}
