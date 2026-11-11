/*
 * Fixture: apply attributes and inferred types to local aggregate pointers.
 */
struct nyc_local_attribute_aggregate {
  void *lock;
};
int main(void) {
  struct nyc_local_attribute_aggregate value = {.lock = (void *)1},
                                       *pointer __attribute__((__unused__)) =
                                           &value;
  __auto_type inferred = pointer;
  if (__builtin_constant_p(inferred) || !__builtin_constant_p(7))
    return 1;
  return inferred->lock != value.lock;
}
