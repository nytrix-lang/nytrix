/*
 * Parser/backend fixture for void-pointer identity and parameter passing.
 */
void *c_void_pointer_sink(void *pointer);

void *c_void_pointer_identity(void *pointer) {
  void *copy = pointer;
  copy = c_void_pointer_sink(copy);
  return copy;
}
