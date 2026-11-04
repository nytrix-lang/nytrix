struct null_base_object {
  unsigned prefix;
  int values[4];
};

#define NULL_BASE_VALUE(index_)                                                \
  (&((struct null_base_object *)0)->values[(index_)])

static int *const selected_value = NULL_BASE_VALUE(2);

int main(void) {
  return (unsigned long)selected_value !=
         __builtin_offsetof(struct null_base_object, values) + 2 * sizeof(int);
}
