/*
 * Fixture: access rows, elements, and fields of an external multidimensional
 * array.
 */
extern const char nyc_matrix[2][4];
const char *nyc_matrix_row(void) { return nyc_matrix[1]; }
int nyc_matrix_value(void) { return nyc_matrix[1][2]; }
struct nyc_multi_field_probe {
  unsigned long values[2][3];
};
unsigned long nyc_multi_field_value(struct nyc_multi_field_probe *probe,
                                    unsigned int row, unsigned int column) {
  return probe->values[row][column];
}
struct nyc_named_member_inner {
  unsigned long value;
};
struct nyc_named_member_outer {
  struct nyc_named_member_inner;
};
unsigned long nyc_named_member_value(struct nyc_named_member_outer *probe) {
  return probe->value;
}
struct nyc_compound_inner {
  unsigned long value;
};
struct nyc_compound_outer {
  struct nyc_compound_inner inner;
};
extern struct nyc_compound_outer nyc_compound_initializer;
extern struct nyc_compound_outer nyc_compound_initializer = {
    .inner = (struct nyc_compound_inner){.value = 31}};
unsigned long nyc_compound_initializer_value(void) {
  return nyc_compound_initializer.inner.value;
}
struct nyc_anonymous_initializer {
  union {
    unsigned long kind;
    void *pointer;
  };
};
struct nyc_anonymous_initializer nyc_anonymous_initializer = {.kind = 37};
unsigned long nyc_anonymous_initializer_value(void) {
  return nyc_anonymous_initializer.kind;
}
typedef unsigned char nyc_thunk_t[32];
extern nyc_thunk_t nyc_thunk_array[2];
const unsigned char *nyc_thunk_row(void) { return nyc_thunk_array[1]; }
typedef unsigned char nyc_nodemask_bytes_t[(1 << 6) / 8];
extern nyc_nodemask_bytes_t nyc_nodemask_bytes;
typedef char nyc_alignment_bound_t[(
    __alignof__(void *) < sizeof(short) ? sizeof(short) : __alignof__(void *))];
extern nyc_alignment_bound_t nyc_alignment_bound;
struct nyc_incomplete_entry;
extern struct nyc_incomplete_entry *nyc_incomplete_entries[];
struct nyc_incomplete_entry *nyc_incomplete_entry_at(unsigned int index) {
  return nyc_incomplete_entries[index];
}
struct nyc_external_object;
extern struct nyc_external_object *nyc_external_object_ptr;
struct nyc_external_object *nyc_read_external_object(void) {
  return nyc_external_object_ptr;
}
extern struct nyc_external_object **nyc_external_object_ptr_ptr;
struct nyc_external_object *nyc_read_external_object_ptr_ptr(void) {
  return *nyc_external_object_ptr_ptr;
}
struct nyc_external_pointer_bucket {
  struct nyc_external_object **items[2];
};
struct nyc_external_object **
nyc_external_pointer_bucket_at(struct nyc_external_pointer_bucket *bucket,
                               unsigned int index) {
  return bucket->items[index];
}
struct nyc_external_aggregate {
  unsigned long value;
};
extern struct nyc_external_aggregate nyc_external_aggregate;
extern struct nyc_external_aggregate nyc_external_aggregate;
unsigned long nyc_read_external_aggregate(void) {
  return nyc_external_aggregate.value;
}
struct nyc_cpumask {
  unsigned long bits[1];
};
typedef struct nyc_cpumask nyc_cpumask_var_t[1];
extern nyc_cpumask_var_t nyc_cpumask_global_rows[2];
const struct nyc_cpumask *nyc_cpumask_global_row(unsigned int index) {
  return nyc_cpumask_global_rows[index];
}
extern char nyc_external_char;
char nyc_read_external_char(void) { return nyc_external_char; }
extern const char *nyc_fixed_string_ptrs[2];
const char *nyc_fixed_string_ptr_at(unsigned int index) {
  return nyc_fixed_string_ptrs[index];
}
extern unsigned long nyc_u64_array[2];
unsigned long nyc_u64_array_read(const unsigned long *values) {
  return values[1];
}
unsigned long nyc_u64_value(void) { return nyc_u64_array[1]; }
unsigned long nyc_u64_array_decay(void) {
  return nyc_u64_array_read(nyc_u64_array);
}
