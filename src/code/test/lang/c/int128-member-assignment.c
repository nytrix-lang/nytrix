/*
 * Fixture: assign scalar values to and bitwise-update aggregate int128 fields.
 */
struct int128_member_holder {
  unsigned __int128 values[2];
};

static struct int128_member_holder holder;

int main(void) {
  holder.values[0] = 1;
  holder.values[0] |= 2;
  holder.values[0] &= 7;
  holder.values[0] ^= 1;
  holder.values[1] = -1;
  const unsigned long *words = (const unsigned long *)&holder;
  return words[0] != 2 || words[1] != 0 || words[2] != (unsigned long)-1 ||
         words[3] != (unsigned long)-1;
}
