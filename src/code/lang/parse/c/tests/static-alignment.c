/*
 * Parser fixture: verify alignment of file-scope and local aggregate objects.
 */
static char alignment_prefix;

struct static_alignment_item {
  char prefix;
  long value;
};

static struct static_alignment_item file_scope_item;

long file_scope_aggregate_alignment(void) {
  return (long)&file_scope_item % 8 + alignment_prefix;
}

long function_scope_aggregate_array_alignment(void) {
  static struct static_alignment_item items[2];
  return (long)items % 8;
}
