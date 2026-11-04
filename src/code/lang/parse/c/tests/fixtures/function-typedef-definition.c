struct nyc_callback_record { int value; };
typedef int (nyc_callback_type)(struct nyc_callback_record *const *);
static nyc_callback_type nyc_callback;
static int nyc_callback(struct nyc_callback_record *const values[]) {
  return values && (*values)->value;
}
int main(void) {
  struct nyc_callback_record record = {7};
  struct nyc_callback_record *pointer = &record;
  return nyc_callback(&pointer) != 1;
}
