/*
 * Fixture: apply _Generic to a conditional expression with pointer types.
 */
void *select_pointer(const char *value) {
  void *selected = _Generic(0 ? value : (void *)1,
      const void *: (void *)value,
      default: (void *)0);
  if (value == 0)
    return 0;
  if (selected == 0)
    return (void *)1;
  return selected;
}
int main(void) {
  const char *value = "pointer identity";
  return select_pointer(value) == value && select_pointer(0) == 0 ? 0 : 1;
}
