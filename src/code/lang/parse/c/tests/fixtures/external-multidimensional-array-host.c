/*
 * Host fixture: define multidimensional arrays and nested aggregate objects.
 */
const char nyc_matrix[2][4] = {{1, 2, 3, 4}, {5, 6, 7, 8}};
struct nyc_multi_field_probe {
  unsigned long values[2][3];
} nyc_multi_field = {{{1, 2, 3}, {4, 5, 6}}};
struct nyc_named_member_inner {
  unsigned long value;
};
struct nyc_named_member_outer {
  struct nyc_named_member_inner;
} nyc_named_member = {{29}};
unsigned char nyc_thunk_array[2][32];
struct nyc_incomplete_entry {
  int value;
} nyc_incomplete_entry_storage;
struct nyc_incomplete_entry *nyc_incomplete_entries[1] = {
    &nyc_incomplete_entry_storage};
struct nyc_external_object {
  int value;
} nyc_external_object_storage;
struct nyc_external_object *nyc_external_object_ptr =
    &nyc_external_object_storage;
struct nyc_external_object **nyc_external_object_ptr_ptr =
    &nyc_external_object_ptr;
struct nyc_external_object **nyc_external_pointer_values[2] = {
    &nyc_external_object_ptr, &nyc_external_object_ptr};
struct nyc_external_pointer_bucket {
  struct nyc_external_object **items[2];
} nyc_external_pointer_bucket = {
    {&nyc_external_object_ptr, &nyc_external_object_ptr}};
struct nyc_external_aggregate {
  unsigned long value;
} nyc_external_aggregate = {23};
struct nyc_cpumask {
  unsigned long bits[1];
};
typedef struct nyc_cpumask nyc_cpumask_var_t[1];
nyc_cpumask_var_t nyc_cpumask_global_rows[2] = {{{11}}, {{19}}};
char nyc_external_char = 42;
const char *nyc_fixed_string_ptrs[2] = {"first", "second"};
unsigned long nyc_u64_array[2] = {11, 19};
const char *nyc_matrix_row(void);
int nyc_matrix_value(void);
unsigned long nyc_multi_field_value(struct nyc_multi_field_probe *,
                                    unsigned int, unsigned int);
unsigned long nyc_named_member_value(struct nyc_named_member_outer *);
const unsigned char *nyc_thunk_row(void);
struct nyc_incomplete_entry *nyc_incomplete_entry_at(unsigned int);
struct nyc_external_object *nyc_read_external_object(void);
struct nyc_external_object *nyc_read_external_object_ptr_ptr(void);
struct nyc_external_object **
nyc_external_pointer_bucket_at(struct nyc_external_pointer_bucket *,
                               unsigned int);
unsigned long nyc_read_external_aggregate(void);
const struct nyc_cpumask *nyc_cpumask_global_row(unsigned int);
unsigned long nyc_compound_initializer_value(void);
unsigned long nyc_anonymous_initializer_value(void);
char nyc_read_external_char(void);
const char *nyc_fixed_string_ptr_at(unsigned int);
unsigned long nyc_u64_value(void);
unsigned long nyc_u64_array_decay(void);
int main(void) {
  return nyc_matrix_row() != nyc_matrix[1] || nyc_matrix_value() != 7 ||
         nyc_multi_field_value(&nyc_multi_field, 1, 2) != 6 ||
         nyc_named_member_value(&nyc_named_member) != 29 ||
         nyc_compound_initializer_value() != 31 ||
         nyc_anonymous_initializer_value() != 37 ||
         nyc_thunk_row() != nyc_thunk_array[1] ||
         nyc_incomplete_entry_at(0) != nyc_incomplete_entries[0] ||
         nyc_read_external_object() != nyc_external_object_ptr ||
         nyc_read_external_object_ptr_ptr() != nyc_external_object_ptr ||
         nyc_external_pointer_bucket_at(&nyc_external_pointer_bucket, 1) !=
             nyc_external_pointer_values[1] ||
         nyc_read_external_aggregate() != 23 ||
         nyc_cpumask_global_row(1) != &nyc_cpumask_global_rows[1][0] ||
         nyc_cpumask_global_row(1)->bits[0] != 19 ||
         nyc_read_external_char() != 42 ||
         nyc_fixed_string_ptr_at(1) != nyc_fixed_string_ptrs[1] ||
         nyc_u64_value() != 19 || nyc_u64_array_decay() != 19;
}
