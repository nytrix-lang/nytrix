/*
 * Fixture: initialize aggregate pointer fields from array-element addresses.
 */
const int sysctl_vals[] = {10, 20};
struct pointer_initializer_field {
  int proc_handler;
  void *extra1;
};
static const struct pointer_initializer_field table[] = {
    {.extra1 = ((void *)&sysctl_vals[0])},
};
int main(void) { return table[0].extra1 != (void *)&sysctl_vals[0]; }
