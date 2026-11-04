struct nyc_auto_pair {
  long left;
  long right;
};

int main(void) {
  struct nyc_auto_pair source = {3, 7};
  __auto_type copy = source;
  source.left = 99;
  if (copy.left != 3 || copy.right != 7 || source.left != 99)
    return 1;
  return 0;
}
