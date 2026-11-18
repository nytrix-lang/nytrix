/*
 * Host-side C counterpart for the static data retention test.
 */
struct operations {
  int (*callback)(int);
};

extern int ignore_dead_operations(int);
extern int call_live_operations(int);
extern const struct operations *asm_operations_reference;
extern const struct operations __start_nyc_retained_operations[];
extern const struct operations __stop_nyc_retained_operations[];
extern const struct operations exported_operations_alias;
extern const struct operations public_operations;

int main(void) {
  if (ignore_dead_operations(5) != 12 || call_live_operations(7) != 18)
    return 1;
  if (!asm_operations_reference || asm_operations_reference->callback(7) != 20)
    return 2;
  if (__stop_nyc_retained_operations - __start_nyc_retained_operations != 1 ||
      __start_nyc_retained_operations[0].callback(7) != 26)
    return 3;
  if (exported_operations_alias.callback(7) != 30 ||
      public_operations.callback(7) != 36)
    return 4;
  return 0;
}
