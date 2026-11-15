/*
 * Fixture: exercise mixed-width multiplication, bitwise operations, and
 * signed comparisons for 128-bit integers.
 */
int main(void) {
  unsigned __int128 product = 3 * (unsigned __int128)5;
  unsigned __int128 bits = product ^ (unsigned __int128)7;
  __int128 negative = (__int128)-2;
  if (bits != 8)
    return 1;
  if (!(negative < (__int128)0))
    return 2;
  if (!(-3 < negative))
    return 3;
  if (!(negative >= -2))
    return 4;
  return 0;
}
