struct empty {};

static struct empty make_empty(void) {
  struct empty value;
  return value;
}

int main(void) {
  struct empty left;
  struct empty right;
  left = right;
  left = make_empty();
  return 0;
}
