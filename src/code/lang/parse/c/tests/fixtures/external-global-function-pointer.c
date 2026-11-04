extern int (*nyc_external_callback)(int);
int nyc_external_callback_call(int value) {
  return nyc_external_callback(value);
}

struct nyc_callback_result {
  int value;
};

extern struct nyc_callback_result *(*nyc_external_struct_callback)(int);
struct nyc_callback_result *nyc_external_struct_callback_call(int value) {
  return nyc_external_struct_callback(value);
}
