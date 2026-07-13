extern int ar_multi_b(int x);

int ar_multi_a(int x) {
  return ar_multi_b(x + 32);
}
