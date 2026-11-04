struct file {
  int value;
};

static void set_value(struct file *file, int value) { file->value = value; }

int main(void) {
  struct file first = {17}, second = {29};
  struct file *files[] = {&first, &second};
  set_value(files[1], 43);
  return first.value != 17 || second.value != 43 ||
         sizeof(files) != 2 * sizeof(void *);
}
