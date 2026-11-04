typedef __builtin_va_list va_list;

struct nyc_uid {
  unsigned int value;
};

static int read_uid(int marker, ...) {
  va_list arguments;
  __builtin_va_start(arguments, marker);
  struct nyc_uid uid = __builtin_va_arg(arguments, struct nyc_uid);
  return marker + uid.value;
}

int main(void) {
  struct nyc_uid uid = {.value = 42};
  return read_uid(7, uid) != 49;
}
