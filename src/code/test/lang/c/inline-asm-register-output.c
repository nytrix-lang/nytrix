/*
 * Runtime fixture for an inline-assembly register output constraint.
 */
static unsigned long read_register_output(void) {
  unsigned long value;
  __asm__ volatile("mov $42, %0" : "=r"(value));
  return value;
}
int main(void) { return read_register_output() != 42; }
