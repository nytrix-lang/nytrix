static int nyc_callback_target(int value) { return value * 3 + 1; }
int (*nyc_external_callback)(int) = nyc_callback_target;
int nyc_external_callback_call(int);

struct nyc_callback_result {
  int value;
};

static struct nyc_callback_result nyc_callback_storage;

static struct nyc_callback_result *nyc_struct_callback_target(int value) {
  nyc_callback_storage.value = value + 11;
  return &nyc_callback_storage;
}

struct nyc_callback_result *(*nyc_external_struct_callback)(int) =
    nyc_struct_callback_target;
struct nyc_callback_result *nyc_external_struct_callback_call(int);

int main(void) {
  if (nyc_external_callback_call(7) != 22)
    return 1;
  struct nyc_callback_result *result = nyc_external_struct_callback_call(31);
  return !result || result->value != 42;
}
