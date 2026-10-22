extern long ar_split_b(long x);

long ar_split_a(long x) {
  return ar_split_b(x + 9);
}
