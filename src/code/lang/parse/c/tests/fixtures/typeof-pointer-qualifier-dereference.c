/*
 * typeof(*pointer) uses the pointee's qualifiers, not top-level pointer const.
 */
static void update_value(unsigned long *const pointer) {
  typeof(*pointer) value = *pointer;
  value += 1;
  *(volatile typeof(*pointer) *)&(*pointer) = value;
}

int main(void) {
  unsigned long value = 41;
  update_value(&value);
  return value != 42;
}
