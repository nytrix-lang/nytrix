/*
 * Runtime fixture for typeof applied to GNU's omitted-middle conditional.
 */
int main(void) {
  unsigned long value = 1;
  unsigned long fallback = 2;
  __typeof__(value ?: fallback) selected = value ?: fallback;
  return sizeof(selected) != sizeof(unsigned long) || selected != 1;
}
