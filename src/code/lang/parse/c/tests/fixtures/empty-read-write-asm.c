/*
 * Fixture: accept an empty inline-assembly template with read-write operands.
 */
static unsigned long preserve(unsigned long value) {
  asm("" : "+rm"(value));
  return value;
}
int main(void) {
  return preserve(0) != 0 || preserve(37) != 37 || preserve(~0UL) != ~0UL;
}
