/*
 * Translation-unit test host: provide standard types and runtime helpers.
 */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#endif

struct nyr_unit_token {
  const char *text;
  unsigned int length;
};
extern int nyr_unit_aggregate_parameter(int, struct nyr_unit_token);
extern int nyr_unit_tag_decl_only_size(void);
extern int nyr_unit_aggregate_array_designators(void);
extern int nyr_unit_extern_array_definition(void);
extern int nyr_unit_global_aggregate_pointer_path(void);
extern int nyr_unit_global_aggregate_pointer_array_call(int value);
extern int nyr_unit_typeof_statement_lvalue_growth(int *value);
extern unsigned long nyr_unit_int128_mul_add_shift(unsigned long value,
                                                   unsigned int multiplier,
                                                   unsigned long addend,
                                                   unsigned int shift);
extern unsigned long nyr_unit_int128_mul_u64_shift(unsigned long value,
                                                   unsigned long multiplier,
                                                   unsigned int shift);

struct nyr_unit_volatile_link {
  struct nyr_unit_volatile_link *next;
};
struct nyr_unit_array_address_probe {
  char bytes[4];
};
struct nyr_unit_global_pointer_item {
  int value;
};
struct nyr_unit_global_pointer_holder {
  struct nyr_unit_global_pointer_item *dev[4];
};
extern struct nyr_unit_global_pointer_holder nyr_unit_global_pointer_holders[2];
extern struct nyr_unit_global_pointer_item nyr_unit_global_pointer_value;
static struct nyr_unit_volatile_link nyr_unit_pointer_matrix_values[6];
struct nyr_unit_volatile_link *nyr_unit_pointer_matrix[3][2] = {
    {&nyr_unit_pointer_matrix_values[0], &nyr_unit_pointer_matrix_values[1]},
    {&nyr_unit_pointer_matrix_values[2], &nyr_unit_pointer_matrix_values[3]},
    {&nyr_unit_pointer_matrix_values[4], &nyr_unit_pointer_matrix_values[5]},
};
struct nyr_unit_volatile_link *nyr_unit_pointer_matrix_expected =
    &nyr_unit_pointer_matrix_values[5];

typedef unsigned long long nyr_unit_u64;
struct nyr_unit_u32_pointer_owner {
  unsigned int *values;
};
struct nyr_unit_array_decay_inner {
  char *name;
};
struct nyr_unit_array_decay_holder {
  struct nyr_unit_array_decay_inner entries[2];
};
extern long nyr_unit_helper(long value);
extern long nyr_unit_entry(long value);
extern char *
nyr_unit_array_decay_assign(struct nyr_unit_array_decay_holder *holder,
                            char *name);
extern int nyr_unit_parenthesized_void_macro(void);
extern int nyr_unit_atomic_slot;
extern int nyr_unit_atomic_roundtrip(int value);
extern void nyr_unit_full_memory_barrier(void);
extern int nyr_unit_fallthrough_attribute(int value);
extern unsigned short nyr_unit_bswap16(unsigned short value);
extern int nyr_unit_clzll(unsigned long long value);
extern int nyr_unit_add_overflow(long long left, long long right,
                                 long long *result);
extern int nyr_unit_sub_overflow(long long left, long long right,
                                 long long *result);
extern int nyr_unit_mul_overflow(long long left, long long right,
                                 long long *result);
extern int nyr_unit_static_function_null(void);
extern int nyr_unit_atexit_register(void);
extern int nyr_unit_variadic_probe(void);
extern int nyr_unit_bitfield_constants(void);
extern long nyr_unit_indexed_pointer_constant(long value);
extern long nyr_unit_signed_short(short value);
extern long nyr_unit_signed_short_load(short *value);
extern long nyr_unit_signed_short_narrow(long value);
extern long nyr_unit_if_else_local_pointer(int choose);
extern int nyr_unit_pid_identity(int process_id);
extern long nyr_unit_enabled_branch(void);
extern int nyr_unit_int_identity(int value);
extern int nyr_unit_int_add(int left, int right);
extern int nyr_unit_hex_e_integer(void);
extern double nyr_unit_long_double_suffix(void);
extern long nyr_unit_int_promote(int value);
extern int nyr_unit_int_narrow(long value);
extern long nyr_unit_int_pointer_roundtrip(int *value);
extern int nyr_unit_int_pointer_index(int *values);
extern unsigned long nyr_unit_sizeof_pointer_array_element(void *owner);
extern double nyr_unit_f64_pointer_field_element(void *owner);
extern unsigned int nyr_unit_u32_pointer_field_element(void *owner, int index);
extern unsigned short *nyr_unit_u16_pointer_field_element_address(void *owner,
                                                                  int index);
extern unsigned short nyr_unit_atomic_u16_load(const unsigned short *value);
extern void nyr_unit_atomic_u16_store(unsigned short *address,
                                      unsigned short value);
extern short nyr_unit_atomic_i16_load(const short *value);
extern int nyr_unit_aggregate_pointer_postincrement(void);
extern unsigned long nyr_unit_builtin_offsetof_expression(void);
extern int nyr_unit_global_aggregate_pointer_array(void);
extern int nyr_unit_string_search(const char *text);
extern int nyr_unit_config_scan(const char *text);
extern int nyr_unit_void_config_scan_result(void);
void nyr_unit_config_record(int matches);
extern int nyr_unit_opaque_pointer_address(void);
extern int nyr_unit_const_aggregate_pointer(void);
extern int nyr_unit_aggregate_array_element_address(void);
extern int nyr_unit_auto_pointer_array(void);
extern int nyr_unit_void_comma_expression(void);
extern int nyr_unit_static_pointer_array(void);
extern int nyr_unit_static_string_array(void);
extern int nyr_unit_int_loop(int count);
struct nyr_unit_pointed_field;
extern int
nyr_unit_conditional_aggregate_pointer(int condition,
                                       struct nyr_unit_pointed_field *left,
                                       struct nyr_unit_pointed_field *right);
extern int nyr_unit_for_comma_initializer(void);
extern long nyr_unit_object_macro(long value);
extern long nyr_unit_self_macro(void);
extern int nyr_unit_char_pointer(char *data);
extern int nyr_unit_string_pointer_array_index(char **values);
extern int nyr_unit_automatic_char_string(void);
extern int nyr_unit_local_enum_scope(int condition);
extern int nyr_unit_string_literal_index(void);
extern int nyr_unit_global_text_at_one(void);
extern int nyr_unit_global_bytes_at_two(void);
extern int nyr_unit_address_extent_matches(void);
extern int nyr_unit_extern_string_pointer_array_at_one(void);
extern int nyr_unit_global_stride_probe(unsigned int index);
extern int nyr_unit_global_char_bytes_at_two(void);
extern int nyr_unit_global_byte_pointer_value(void);
extern const char *nyr_unit_string_pointer_table_at_one(void);
extern const char nyr_unit_global_text[];
extern int nyr_unit_global_i32_read(void);
extern unsigned int nyr_unit_global_u32_read(void);
extern unsigned char nyr_unit_global_u8_read(void);
extern long nyr_unit_global_i64_read(void);
extern double nyr_unit_global_f64_read(void);
extern int nyr_unit_host_global_read(void);
extern void nyr_unit_global_write(void);
extern int nyr_unit_global_aggregate_update(void);
extern int nyr_unit_alias_local_value(void);
extern int nyr_unit_static_aggregate_update(void);
struct nyr_unit_mutable_global;
extern struct nyr_unit_mutable_global *nyr_unit_static_aggregate_address(void);
extern int nyr_unit_static_aggregate_array_count(void);
extern int nyr_unit_variadic_call(void);
extern int nyr_unit_variadic_sum(int tag, ...);
extern int nyr_unit_variadic_copy(int tag, ...);
extern int nyr_unit_switch(int value);
extern int nyr_unit_switch_range(int value);
extern int nyr_unit_switch_enum_case(int value);
extern int nyr_unit_statement_expression_braced_if(int value);
extern char *
nyr_unit_address_of_array_member(struct nyr_unit_array_address_probe *probe);
extern int nyr_unit_choose_expr(int value);
extern void *nyr_unit_tied_asm_pointer(void *value);
extern int nyr_unit_kernel_order_base_immediate(void);
extern int nyr_unit_switch_nested_label(int value);
extern int nyr_unit_switch_returns(int value);
extern int nyr_unit_switch_goto_label(int value);
struct nyr_unit_typed_pointer_field {
  long *value;
};
extern long
nyr_unit_typed_pointer_field_read(struct nyr_unit_typed_pointer_field *field);
extern long
nyr_unit_pointer_field_index_write(struct nyr_unit_typed_pointer_field *field,
                                   long value);
extern int nyr_unit_chained_local_array_assignment(void);
int nyr_unit_host_global = 5;
int nyr_unit_block_extern_data = 73;
extern int nyr_unit_block_extern_read(void);
extern int nyr_unit_block_extern_add(int value);
extern int nyr_unit_unsigned_pointer_order(void);
extern int nyr_unit_address_taken_double_call(void);
extern int nyr_unit_indirect_double_result_call(void);
extern int nyr_unit_indirect_i32_negative_result(void);

int nyr_unit_host_variadic_probe(int tag, ...) {
  va_list args;
  va_start(args, tag);
  double real = va_arg(args, double);
  int integer = va_arg(args, int);
  va_end(args);
  return tag == 3 && real == 2.5 && integer == 7 ? 42 : 0;
}
extern int nyr_unit_unsigned_compare(nyr_unit_u64 left, nyr_unit_u64 right);
extern nyr_unit_u64 nyr_unit_unsigned_add(nyr_unit_u64 left,
                                          nyr_unit_u64 right);
extern nyr_unit_u64 nyr_unit_unsigned_div(nyr_unit_u64 value,
                                          nyr_unit_u64 divisor);
extern nyr_unit_u64 nyr_unit_unsigned_mod(nyr_unit_u64 value,
                                          nyr_unit_u64 divisor);
extern unsigned int nyr_unit_unsigned_int_div(unsigned int value,
                                              unsigned int divisor);
extern unsigned int nyr_unit_unsigned_int_mod(unsigned int value,
                                              unsigned int divisor);
extern nyr_unit_u64 nyr_unit_unsigned_shift(nyr_unit_u64 value,
                                            unsigned int count);
extern nyr_unit_u64 nyr_unit_unsigned_max(void);
extern int nyr_unit_for_comma_updates(void);
extern unsigned int nyr_unit_u32_add(unsigned int left, unsigned int right);
extern int nyr_unit_u32_compare(unsigned int left, unsigned int right);
struct nyr_unit_u32_adjacent {
  unsigned int count;
  void *pointer;
};
extern unsigned int
nyr_unit_u32_field_increment(struct nyr_unit_u32_adjacent *value);
static int test_u32_field_increment(void) {
  int marker = 0;
  struct nyr_unit_u32_adjacent adjacent = {4, &marker};
  unsigned int previous = nyr_unit_u32_field_increment(&adjacent);
  return previous == 4 && adjacent.count == 5 && adjacent.pointer == &marker;
}
extern unsigned char nyr_unit_u8_increment(unsigned char value);
extern char nyr_unit_char_identity(char value);
typedef int (*nyr_unit_callback)(int);
extern nyr_unit_callback nyr_unit_callback_identity(nyr_unit_callback callback);
extern int nyr_unit_callback_parameter(nyr_unit_callback callback, int value);
extern int nyr_unit_discard_callback_result(nyr_unit_callback callback,
                                            int value);
extern char **nyr_unit_pointer_pointer_identity(char **value);
extern char **nyr_unit_pointer_pointer_predecrement(char **value);
extern char **nyr_unit_pointer_pointer_postdecrement(char **value);
extern int nyr_unit_return_with_unreachable_statement(int value);
extern int nyr_unit_return_with_unreachable_label(int value);
extern char ***nyr_unit_pointer_triple_identity(char ***value);
struct nyr_unit_mixed {
  int code;
  long value;
};
struct nyr_unit_mutable_global {
  int value;
};
struct nyr_unit_volatile_field {
  volatile int value;
};
struct nyr_unit_callback_field {
  void (*callback)(void);
};
struct nyr_unit_callback_pointer_array {
  void (**items)(void);
};
struct nyr_unit_pointed_field {
  int value;
};
struct nyr_unit_pointer_to_aggregate_field {
  struct nyr_unit_pointed_field *items;
};
struct nyr_unit_aggregate_pointer_slot {
  struct nyr_unit_aggregate_pointer_slot *next;
};
struct nyr_unit_char_pointer_slots {
  char **items;
};
struct nyr_unit_single_char_array {
  char value[1];
};
extern void nyr_unit_set_callback_field(struct nyr_unit_callback_field *state,
                                        void (*callback)(void));
extern void nyr_unit_call_callback_field(struct nyr_unit_callback_field *state);
extern int nyr_unit_callback_pointer_array_equal(
    struct nyr_unit_callback_pointer_array *state, void (*expected)(void));
extern void
nyr_unit_set_pointed_field(struct nyr_unit_pointer_to_aggregate_field *state,
                           int value);
extern void nyr_unit_set_aggregate_pointer_slot(
    struct nyr_unit_aggregate_pointer_slot *state,
    struct nyr_unit_aggregate_pointer_slot *value);
extern char *
nyr_unit_char_pointer_slot(struct nyr_unit_char_pointer_slots *state,
                           int index);
extern char *
nyr_unit_single_char_array_decay(struct nyr_unit_single_char_array *value);
extern int
nyr_unit_volatile_field_update(struct nyr_unit_volatile_field *state);
extern int nyr_unit_inline_aggregate_array(void);
extern int nyr_unit_null_opaque_pointer(void);
extern int nyr_unit_null_macro(void);
extern char *nyr_unit_static_null_pointer(void);
extern int nyr_unit_auto_char_array(void);
extern int nyr_unit_pointer_difference(char *bytes);
struct nyr_unit_addressed_bytes {
  unsigned char *bytes;
  int index;
};
struct nyr_unit_aggregate_pointer_chain {
  struct nyr_unit_aggregate_pointer_chain **previous;
  struct nyr_unit_aggregate_pointer_chain *next;
};
extern int
nyr_unit_parenthesized_member_address(struct nyr_unit_addressed_bytes *page,
                                      int index);
extern int
nyr_unit_prefix_aggregate_decrement(struct nyr_unit_addressed_bytes *page);
extern int nyr_unit_call_result_member_assignment(int value);
extern void nyr_unit_aggregate_pointer_chain_assign(
    struct nyr_unit_aggregate_pointer_chain *node);
extern int nyr_unit_conditional_comma(int condition, int *value);
extern double nyr_unit_integer_to_double(long value);
extern double nyr_unit_volatile_pointer_and_local(volatile double *values);
extern double nyr_unit_volatile_parameter(volatile double value);
extern int nyr_unit_mixed_code(struct nyr_unit_mixed *value);
extern long nyr_unit_mixed_value(struct nyr_unit_mixed *value);
extern void nyr_unit_mixed_set_code(struct nyr_unit_mixed *value, int code);
struct nyr_unit_array_fields {
  unsigned char bytes[4];
  unsigned int words[2];
};
extern unsigned int
nyr_unit_array_field_access(struct nyr_unit_array_fields *value);
struct nyr_unit_nested_inner {
  long values[2];
};
struct nyr_unit_nested_outer {
  struct nyr_unit_nested_inner items[2];
};
extern struct nyr_unit_nested_outer nyr_unit_nested_global;
struct nyr_unit_float_layout {
  char tag;
  float values[2];
  int code;
};
extern struct nyr_unit_float_layout nyr_unit_float_layout_global;
struct nyr_unit_float_entry {
  unsigned char name_len;
  char name[7];
  float limit;
  float scale;
};
extern struct nyr_unit_float_entry nyr_unit_float_entries[];
struct nyr_unit_opaque;
extern struct nyr_unit_opaque *
nyr_unit_opaque_identity(struct nyr_unit_opaque *value);

static int callback_field_calls;
static int config_scan_matches;

static void callback_field_target(void) { callback_field_calls++; }
void nyr_unit_config_record(int matches) { config_scan_matches = matches; }

static int callback_parameter_target(int value) { return value + 7; }

int nyr_unit_signed_bytes(int value);
long nyr_unit_scoped_local_classes(int branch, double input, long *pointer);
int nyr_unit_short_lvalue_stores(void);
void *nyr_unit_allocation_header(int size);
struct nyr_unit_link {
  struct nyr_unit_link *next;
  int value;
};
int nyr_unit_parameter_loop(struct nyr_unit_link *node);
int nyr_unit_static_link_value(struct nyr_unit_link *node);
int nyr_unit_out_parameter(void);
unsigned char *nyr_unit_static_byte(void);
int nyr_unit_static_counter(void);
int nyr_unit_global_sizeof(void);
int nyr_unit_whole_array_address(void);
int nyr_unit_loop_storage(void);
int nyr_unit_stdio(int value);
int nyr_unit_variadic_sink(const char *format, ...) {
  if (strcmp(format, "%f") != 0)
    return -1;
  va_list arguments;
  va_start(arguments, format);
  double value = va_arg(arguments, double);
  va_end(arguments);
  return value == 1.25 ? 0 : -1;
}
void nyr_unit_write_int(int *value) { *value = 42; }
void nyr_unit_fill_bytes(void *value, int count) {
  unsigned char *bytes = value;
  for (int i = 0; i < count; ++i)
    bytes[i] = 165;
}

int main(void) {
  if (nyr_unit_block_extern_read() != 73) {
    fprintf(stderr, "block-scope extern did not resolve to external data\n");
    return 93;
  }
  if (nyr_unit_block_extern_add(5) != 78 ||
      nyr_unit_block_extern_read() != 78) {
    fprintf(stderr, "block-scope extern compound assignment failed\n");
    return 98;
  }
  if (!nyr_unit_unsigned_pointer_order()) {
    fprintf(stderr, "pointer relational comparison was not unsigned\n");
    return 99;
  }
  if (!nyr_unit_address_taken_double_call()) {
    fprintf(stderr, "address-taken double call argument was misclassified\n");
    return 100;
  }
  if (!nyr_unit_indirect_double_result_call()) {
    fprintf(stderr, "indirect double call result was misclassified\n");
    return 101;
  }
  if (!nyr_unit_indirect_i32_negative_result()) {
    fprintf(stderr, "indirect signed int call result was not sign-extended\n");
    return 102;
  }
  if (nyr_unit_parenthesized_void_macro() != 23) {
    fprintf(stderr, "parenthesized void macro call was not executed\n");
    return 90;
  }
  if (nyr_unit_static_null_pointer() != NULL) {
    fprintf(stderr, "static NULL pointer initializer was not zero\n");
    return 91;
  }
  if (nyr_unit_atexit_register() != 0) {
    fprintf(stderr, "atexit registration failed\n");
    return 92;
  }
  if (nyr_unit_atomic_roundtrip(37) != 37 || nyr_unit_atomic_slot != 37) {
    fprintf(stderr, "relaxed atomic store/load roundtrip failed\n");
    return 94;
  }
  nyr_unit_full_memory_barrier();
  if (nyr_unit_fallthrough_attribute(1) != 3 ||
      nyr_unit_fallthrough_attribute(2) != 2 ||
      nyr_unit_fallthrough_attribute(0) != 0) {
    fprintf(stderr, "fallthrough attribute changed switch behavior\n");
    return 95;
  }
  if (nyr_unit_bswap16(0x1234) != 0x3412 ||
      nyr_unit_bswap16(0x00ff) != 0xff00) {
    fprintf(stderr, "16-bit byte-swap builtin returned the wrong value\n");
    return 96;
  }
  if (nyr_unit_clzll(1ULL) != 63 || nyr_unit_clzll(1ULL << 63) != 0 ||
      nyr_unit_clzll(0x10000ULL) != 47) {
    fprintf(stderr, "64-bit leading-zero builtin returned the wrong value\n");
    return 97;
  }
  long long overflow_result = 0;
  if (nyr_unit_add_overflow(INT64_MAX, 1, &overflow_result) != 1 ||
      overflow_result != INT64_MIN ||
      nyr_unit_add_overflow(40, 2, &overflow_result) != 0 ||
      overflow_result != 42 ||
      nyr_unit_sub_overflow(INT64_MIN, 1, &overflow_result) != 1 ||
      overflow_result != INT64_MAX ||
      nyr_unit_sub_overflow(42, 2, &overflow_result) != 0 ||
      overflow_result != 40 ||
      nyr_unit_mul_overflow(INT64_MAX, 2, &overflow_result) != 1 ||
      overflow_result != -2 ||
      nyr_unit_mul_overflow(INT64_MIN, -1, &overflow_result) != 1 ||
      overflow_result != INT64_MIN ||
      nyr_unit_mul_overflow(42, 2, &overflow_result) != 0 ||
      overflow_result != 84) {
    fprintf(stderr, "64-bit overflow builtins returned the wrong result\n");
    return 98;
  }
  if (nyr_unit_static_function_null() != 1) {
    fprintf(stderr, "static function-pointer null initializer was not zero\n");
    return 99;
  }
  struct nyr_unit_array_decay_holder array_decay = {0};
  char *array_decay_name = "first-element";
  if (nyr_unit_array_decay_assign(&array_decay, array_decay_name) !=
          array_decay_name ||
      array_decay.entries[0].name != array_decay_name) {
    fprintf(stderr,
            "aggregate array field did not decay to its first element\n");
    return 93;
  }
  long scoped_value = 23;
  if (nyr_unit_global_aggregate_pointer_array() != 42)
    return 1;
  if (nyr_unit_global_aggregate_pointer_array_call(23) != 42)
    return 1;
  if (nyr_unit_typeof_statement_lvalue_growth(NULL) != 7)
    return 1;
  if (!nyr_unit_string_search("prefix CONFIG_VALUE") ||
      nyr_unit_string_search("prefix_CONFIG_VALUE") ||
      nyr_unit_string_search("missing"))
    return 1;
  if (nyr_unit_config_scan("/* CONFIG_A prefix CONFIG_B and _CONFIG_C */") != 2)
    return 1;
  if (nyr_unit_void_config_scan_result() != 1 || config_scan_matches != 2)
    return 1;
  if (nyr_unit_variadic_probe() != 0)
    return 1;
  if (nyr_unit_stdio(42) != 17)
    return 1;
  if (nyr_unit_loop_storage() != 1351680)
    return 1;
  if (!nyr_unit_whole_array_address())
    return 1;
  if (!nyr_unit_global_sizeof())
    return 1;
  unsigned char *static_byte = nyr_unit_static_byte();
  if (*static_byte != 17)
    return 1;
  *static_byte = 23;
  if (nyr_unit_static_byte() != static_byte || *nyr_unit_static_byte() != 23 ||
      nyr_unit_static_counter() != 41 || nyr_unit_static_counter() != 42)
    return 1;
  if (!nyr_unit_out_parameter())
    return 1;
  struct nyr_unit_link tail = {NULL, 40};
  struct nyr_unit_link head = {&tail, 7};
  if (nyr_unit_static_link_value(&head) != 7 ||
      nyr_unit_static_link_value(NULL) != 7 ||
      nyr_unit_static_link_value(&tail) != 40 ||
      nyr_unit_static_link_value(NULL) != 40)
    return 1;
  if (nyr_unit_parameter_loop(&head) != 40)
    return 1;
  long *allocation = nyr_unit_allocation_header(130);
  if (!allocation || allocation[-1] != 130)
    return 1;
  free(allocation - 1);
  if (!nyr_unit_short_lvalue_stores())
    return 1;
  if (!nyr_unit_bitfield_constants()) {
    fprintf(stderr, "C bitfield or pointer constant initializer mismatch\n");
    return 1;
  }
  if (nyr_unit_indexed_pointer_constant(1000) != 1000 ||
      nyr_unit_indexed_pointer_constant(0) != 0) {
    fprintf(stderr, "C address-of evaluated the pointed-to object\n");
    return 1;
  }
  short narrow_values[] = {-32768, -1, 0, 32767};
  for (unsigned i = 0; i < sizeof(narrow_values) / sizeof(narrow_values[0]);
       ++i) {
    if (nyr_unit_signed_short(narrow_values[i]) != narrow_values[i] ||
        nyr_unit_signed_short_load(&narrow_values[i]) != narrow_values[i]) {
      fprintf(stderr, "C signed-short sign extension mismatch\n");
      return 1;
    }
  }
  if (nyr_unit_signed_short_narrow(65535) != -1 ||
      nyr_unit_signed_short_narrow(32768) != -32768) {
    fprintf(stderr, "C signed-short narrowing mismatch\n");
    return 1;
  }
  void (*callback_items[])(void) = {callback_field_target};
  struct nyr_unit_callback_pointer_array callback_pointer_array = {
      callback_items};
  if (!nyr_unit_callback_pointer_array_equal(&callback_pointer_array,
                                             callback_field_target))
    return 1;
  struct nyr_unit_aggregate_pointer_slot pointer_slot = {0};
  struct nyr_unit_aggregate_pointer_slot pointer_value = {0};
  nyr_unit_set_aggregate_pointer_slot(&pointer_slot, &pointer_value);
  if (pointer_slot.next != &pointer_value)
    return 1;
  struct nyr_unit_single_char_array single_char_array = {{'x'}};
  char *decayed_single_char_array =
      nyr_unit_single_char_array_decay(&single_char_array);
  if (decayed_single_char_array != single_char_array.value ||
      decayed_single_char_array[0] != 'x')
    return 1;
  struct nyr_unit_callback_field callback_field = {0};
  void (*expected_callback)(void) = (void (*)(void))(uintptr_t)0x12340;
  nyr_unit_set_callback_field(&callback_field, expected_callback);
  struct nyr_unit_callback_field callable_callback_field = {
      callback_field_target};
  struct nyr_unit_pointed_field pointed_fields[2] = {{3}, {4}};
  struct nyr_unit_pointer_to_aggregate_field pointer_to_aggregate_field = {
      pointed_fields};
  nyr_unit_set_pointed_field(&pointer_to_aggregate_field, 17);
  int variadic_sum = nyr_unit_variadic_sum(1, 2, 3.0, 4L, 5, 6, 7, 8);
  if (variadic_sum != 33) {
    fprintf(stderr, "variadic sum: expected 33, got %d\n", variadic_sum);
    return 7;
  }
  int variadic_copy = nyr_unit_variadic_copy(1, 10);
  if (variadic_copy != 21) {
    fprintf(stderr, "variadic copy: expected 21, got %d\n", variadic_copy);
    return 8;
  }
  if (nyr_unit_switch(1) != 5 || nyr_unit_switch(2) != 3 ||
      nyr_unit_switch(9) != 7 || nyr_unit_switch(';') != 11 ||
      nyr_unit_switch('\f') != 12) {
    fprintf(stderr, "switch dispatch or fallthrough result mismatch\n");
    return 9;
  }
  int switch_range_before = nyr_unit_switch_range('/');
  int switch_range_first = nyr_unit_switch_range('0');
  int switch_range_last = nyr_unit_switch_range('7');
  int switch_range_after = nyr_unit_switch_range('8');
  if (switch_range_before != 0 || switch_range_first != 1 ||
      switch_range_last != 1 || switch_range_after != 2) {
    fprintf(stderr, "GNU switch case range boundary mismatch: %d %d %d %d\n",
            switch_range_before, switch_range_first, switch_range_last,
            switch_range_after);
    return 9;
  }
  if (nyr_unit_switch_enum_case(10) != 1 ||
      nyr_unit_switch_enum_case(20) != 2 ||
      nyr_unit_switch_enum_case(22) != 2 ||
      nyr_unit_switch_enum_case(23) != 0) {
    fprintf(stderr, "enum-valued switch case expression mismatch\n");
    return 9;
  }
  if (nyr_unit_statement_expression_braced_if(0) != 0 ||
      nyr_unit_statement_expression_braced_if(1) != 17) {
    fprintf(stderr, "braced if in GNU statement expression failed\n");
    return 9;
  }
  struct nyr_unit_array_address_probe array_probe = {0};
  if (nyr_unit_address_of_array_member(&array_probe) != &array_probe.bytes[0]) {
    fprintf(stderr, "address of aggregate array member was incorrect\n");
    return 10;
  }
  if (nyr_unit_choose_expr(-17) != 17 || nyr_unit_choose_expr(5) != 5) {
    fprintf(stderr, "GNU choose-expression branch selection failed\n");
    return 11;
  }
  int asm_pointer_marker = 0;
  if (nyr_unit_tied_asm_pointer(&asm_pointer_marker) != &asm_pointer_marker ||
      nyr_unit_tied_asm_pointer(NULL) != NULL) {
    fprintf(stderr, "tied register inline asm changed its pointer operand\n");
    return 9;
  }
  if (nyr_unit_kernel_order_base_immediate() != 6) {
    fprintf(stderr, "Linux order_base_2 immediate expression failed\n");
    return 9;
  }
  if (nyr_unit_switch_nested_label(1) != 6 ||
      nyr_unit_switch_nested_label(3) != 5 ||
      nyr_unit_switch_nested_label(9) != 7) {
    fprintf(stderr, "nested switch label dispatch result mismatch\n");
    return 9;
  }
  if (nyr_unit_switch_returns(1) != 5 || nyr_unit_switch_returns(2) != 7 ||
      nyr_unit_switch_returns(9) != 9) {
    fprintf(stderr, "all-returning switch result mismatch\n");
    return 9;
  }
  if (nyr_unit_switch_goto_label(1) != 11 ||
      nyr_unit_switch_goto_label(8) != 12) {
    fprintf(stderr, "goto to a label after a switch-case terminator failed\n");
    return 9;
  }
  long pointer_values[] = {23, 0};
  struct nyr_unit_typed_pointer_field typed_pointer_field = {pointer_values};
  if (nyr_unit_typed_pointer_field_read(&typed_pointer_field) != 23 ||
      nyr_unit_pointer_field_index_write(&typed_pointer_field, 31) != 31 ||
      pointer_values[1] != 31 ||
      nyr_unit_chained_local_array_assignment() != -2) {
    fprintf(stderr, "typed pointer field or local array assignment mismatch\n");
    return 9;
  }

  char bytes[] = {'a', 'b', 'c'};
  unsigned char addressed_bytes[] = {4, 8, 15};
  struct nyr_unit_addressed_bytes page = {addressed_bytes, 3};
  struct nyr_unit_aggregate_pointer_chain next_node = {0};
  struct nyr_unit_aggregate_pointer_chain node = {&node.next, &next_node};
  nyr_unit_aggregate_pointer_chain_assign(&node);
  if (next_node.previous != node.previous) {
    fprintf(stderr, "aggregate pointer chain assignment failed\n");
    return 10;
  }
  int comma_value = 0;
  if (nyr_unit_conditional_comma(1, &comma_value) != 1 || comma_value != 7) {
    fprintf(stderr, "conditional comma expression failed\n");
    return 11;
  }
  if (nyr_unit_for_comma_initializer() != 9) {
    fprintf(stderr, "for-loop comma initializer failed\n");
    return 12;
  }
  int pointed_int = 17;
  int signed_pointed_int = INT32_MIN;
  int pointed_ints[] = {9, 0};
  char *strings[] = {"one", "two"};
  char signed_chars[] = {'x', (char)0x80};
  char *nested_strings[] = {"unused", signed_chars};
  struct nyr_unit_char_pointer_slots char_pointer_slots = {strings};
  char ***string_groups = (char ***)0;
  struct nyr_unit_mixed mixed = {-17, 123};
  struct nyr_unit_volatile_field volatile_field = {7};
  struct nyr_unit_pointed_field conditional_left = {31};
  struct nyr_unit_pointed_field conditional_right = {47};
  double volatile_values[] = {1.25, 2.5};
  double pointer_field_values[] = {1.25};
  struct {
    double *values;
  } f64_pointer_owner = {pointer_field_values};
  if (nyr_unit_f64_pointer_field_element(&f64_pointer_owner) != 1.25) {
    fprintf(stderr, "C double-pointer field indexing mismatch\n");
    return 1;
  }
#if defined(__linux__)
  long page_size = sysconf(_SC_PAGESIZE);
  if (page_size <= (long)sizeof(unsigned int)) {
    fprintf(stderr, "cannot create unsigned-pointer guard-page test\n");
    return 1;
  }
  size_t mapping_size = (size_t)page_size * 2;
  void *guard_mapping = mmap(NULL, mapping_size, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (guard_mapping == MAP_FAILED ||
      mprotect((char *)guard_mapping + page_size, (size_t)page_size,
               PROT_NONE) != 0) {
    if (guard_mapping != MAP_FAILED)
      munmap(guard_mapping, mapping_size);
    fprintf(stderr, "cannot protect unsigned-pointer guard page\n");
    return 1;
  }
  unsigned int *last_u32 =
      (unsigned int *)((char *)guard_mapping + page_size - sizeof(*last_u32));
  *last_u32 = UINT32_C(0x89abcdef);
  struct nyr_unit_u32_pointer_owner u32_pointer_owner = {last_u32};
  int u32_pointer_load_ok = nyr_unit_u32_pointer_field_element(
                                &u32_pointer_owner, 0) == UINT32_C(0x89abcdef);
  unsigned short *last_u16 =
      (unsigned short *)((char *)guard_mapping + page_size - sizeof(*last_u16));
  *last_u16 = 0x1234;
  int u16_atomic_load_ok = nyr_unit_atomic_u16_load(last_u16) == 0x1234;
  nyr_unit_atomic_u16_store(last_u16, 0x5678);
  u16_atomic_load_ok &= nyr_unit_atomic_u16_load(last_u16) == 0x5678;
  short i16_atomic_value = -12345;
  int i16_atomic_load_ok =
      nyr_unit_atomic_i16_load(&i16_atomic_value) == -12345;
  unsigned short u16_pointer_values[] = {0x1234, 0x5678};
  struct {
    unsigned short *values;
  } u16_pointer_owner = {u16_pointer_values};
  int u16_pointer_address_ok =
      nyr_unit_u16_pointer_field_element_address(&u16_pointer_owner, 1) ==
      &u16_pointer_values[1];
  int unmap_status = munmap(guard_mapping, mapping_size);
  if (!u32_pointer_load_ok || !u16_atomic_load_ok || !i16_atomic_load_ok ||
      !u16_pointer_address_ok || unmap_status != 0) {
    fprintf(stderr,
            "C typed pointer-field access failed (u32=%d u16-atomic=%d "
            "i16-atomic=%d u16-address=%d unmap=%d)\n",
            u32_pointer_load_ok, u16_atomic_load_ok, i16_atomic_load_ok,
            u16_pointer_address_ok, unmap_status);
    return 1;
  }
#endif
  struct nyr_unit_array_fields array_fields = {{1, 2, 3, 4}, {7, 0}};
  nyr_unit_callback callback = (nyr_unit_callback)(uintptr_t)0x56780;
  struct nyr_unit_opaque *opaque = (struct nyr_unit_opaque *)(uintptr_t)0x12340;
  return nyr_unit_helper(13) == 39 && nyr_unit_entry(13) == 40 &&
                 nyr_unit_if_else_local_pointer(1) == 41 &&
                 nyr_unit_if_else_local_pointer(0) == 42 &&
                 nyr_unit_pid_identity(1234) == 1234 &&
                 nyr_unit_enabled_branch() == 42 &&
                 nyr_unit_int_identity(-23) == -23 &&
                 nyr_unit_int_add(12, 9) == 24 &&
                 nyr_unit_hex_e_integer() == 224 &&
                 nyr_unit_int_promote(-17) == -17 &&
                 nyr_unit_int_narrow(4294967297L) == 1 &&
                 nyr_unit_int_narrow(2147483648L) == (-2147483647 - 1) &&
                 nyr_unit_long_double_suffix() == 1.25 &&
                 nyr_unit_int_pointer_roundtrip(&pointed_int) == 17 &&
                 pointed_int == 21 &&
                 nyr_unit_int_pointer_roundtrip(&signed_pointed_int) ==
                     INT32_MIN &&
                 signed_pointed_int == INT32_MIN + 4 &&
                 nyr_unit_int_pointer_index(pointed_ints) == 14 &&
                 nyr_unit_sizeof_pointer_array_element(NULL) ==
                     sizeof(void *) &&
                 nyr_unit_builtin_offsetof_expression() == offsetof(
                                                               struct {
                                                                 char prefix;
                                                                 long value;
                                                               },
                                                               value) &&
                 nyr_unit_opaque_pointer_address() == 1 &&
                 nyr_unit_const_aggregate_pointer() == 0 &&
                 nyr_unit_aggregate_array_element_address() == 42 &&
                 nyr_unit_conditional_aggregate_pointer(
                     1, &conditional_left, &conditional_right) == 31 &&
                 nyr_unit_conditional_aggregate_pointer(
                     0, &conditional_left, &conditional_right) == 47 &&
                 nyr_unit_auto_pointer_array() == 'o' + 't' &&
                 nyr_unit_void_comma_expression() == 7 &&
                 nyr_unit_static_pointer_array() == 'B' + 'R' &&
                 pointed_ints[1] == 14 && nyr_unit_static_string_array() == 9 &&
                 nyr_unit_int_loop(10) == 45 &&
                 nyr_unit_object_macro(13) == 52 &&
                 nyr_unit_self_macro() == 7 &&
                 nyr_unit_global_text_at_one() == 'b' &&
                 nyr_unit_global_bytes_at_two() == 255 &&
                 nyr_unit_extern_string_pointer_array_at_one() == 'b' &&
                 nyr_unit_global_stride_probe(1) == 42 &&
                 nyr_unit_global_stride_probe(2) == 81 &&
                 nyr_unit_global_char_bytes_at_two() == 2 * 'c' &&
                 nyr_unit_global_byte_pointer_value() == 255 &&
                 nyr_unit_string_pointer_table_at_one()[0] == 'b' &&
                 nyr_unit_global_i32_read() == -17 &&
                 nyr_unit_global_u32_read() == UINT32_MAX &&
                 nyr_unit_global_u8_read() == 250 &&
                 nyr_unit_global_i64_read() == 0 &&
                 nyr_unit_global_f64_read() == 1.5 &&
                 nyr_unit_host_global_read() == 5 &&
                 (nyr_unit_global_write(),
                  nyr_unit_global_i32_read() == 29 &&
                      nyr_unit_global_u32_read() == 7 &&
                      nyr_unit_global_u8_read() == 9 &&
                      nyr_unit_global_i64_read() == 41 &&
                      nyr_unit_global_f64_read() == 2.5 &&
                      nyr_unit_host_global_read() == 13) &&
                 nyr_unit_global_aggregate_update() == 8 &&
                 nyr_unit_global_aggregate_update() == 9 &&
                 nyr_unit_aggregate_pointer_postincrement() &&
                 nyr_unit_alias_local_value() == 12 &&
                 nyr_unit_static_aggregate_update() == 5 &&
                 nyr_unit_static_aggregate_update() == 6 &&
                 nyr_unit_static_aggregate_address()->value == 73 &&
                 nyr_unit_static_aggregate_address() ==
                     nyr_unit_static_aggregate_address() &&
                 nyr_unit_static_aggregate_array_count() == 3 &&
                 nyr_unit_variadic_call() == 42 &&
                 nyr_unit_global_text[0] == 'a' &&
                 nyr_unit_global_text[2] == 'c' &&
                 nyr_unit_global_text[3] == '\0' &&
                 nyr_unit_unsigned_compare(UINT64_MAX, UINT64_MAX - 1) &&
                 nyr_unit_unsigned_add(UINT64_MAX, 1) == 0 &&
                 nyr_unit_unsigned_div(UINT64_MAX, 10) == UINT64_MAX / 10 &&
                 nyr_unit_unsigned_mod(UINT64_MAX, 10) == UINT64_MAX % 10 &&
                 nyr_unit_unsigned_int_div(UINT32_MAX, 3) == UINT32_MAX / 3 &&
                 nyr_unit_unsigned_int_mod(UINT32_MAX, 3) == UINT32_MAX % 3 &&
                 nyr_unit_string_literal_index() == 'Z' &&
                 nyr_unit_unsigned_shift(UINT64_MAX, 33) ==
                     (UINT64_MAX >> 33) &&
                 nyr_unit_unsigned_max() == UINT64_MAX &&
                 nyr_unit_for_comma_updates() == 5 &&
                 nyr_unit_u32_add(0xffffffffU, 1U) == 0 &&
                 nyr_unit_u32_compare(0xffffffffU, 1U) &&
                 test_u32_field_increment() &&
                 nyr_unit_u8_increment(255) == 0 &&
                 nyr_unit_char_identity(65) == 65 &&
                 nyr_unit_callback_identity(callback) == callback &&
                 nyr_unit_callback_parameter(callback_parameter_target, 9) ==
                     16 &&
                 nyr_unit_discard_callback_result(callback_parameter_target,
                                                  9) == 1 &&
                 callback_field.callback == expected_callback &&
                 (nyr_unit_call_callback_field(&callable_callback_field),
                  callback_field_calls == 1) &&
                 pointed_fields[1].value == 17 &&
                 nyr_unit_char_pointer_slot(&char_pointer_slots, 1) ==
                     strings[1] &&
                 nyr_unit_pointer_pointer_identity(strings) == strings &&
                 nyr_unit_pointer_pointer_predecrement(strings + 1) ==
                     strings &&
                 nyr_unit_pointer_pointer_postdecrement(strings + 1) ==
                     strings &&
                 nyr_unit_return_with_unreachable_statement(42) == 42 &&
                 nyr_unit_return_with_unreachable_label(0) == 17 &&
                 nyr_unit_return_with_unreachable_label(42) == 42 &&
                 nyr_unit_string_pointer_array_index(nested_strings) == -128 &&
                 nyr_unit_automatic_char_string() &&
                 nyr_unit_local_enum_scope(1) == 7 &&
                 nyr_unit_local_enum_scope(0) == 17 &&
                 nyr_unit_pointer_triple_identity(string_groups) ==
                     string_groups &&
                 nyr_unit_mixed_code(&mixed) == -17 &&
                 nyr_unit_inline_aggregate_array() == 7 &&
                 nyr_unit_volatile_field_update(&volatile_field) == 7 &&
                 volatile_field.value == 7 &&
                 nyr_unit_volatile_pointer_and_local(volatile_values) == 3.75 &&
                 nyr_unit_volatile_parameter(2.5) == 3.5 &&
                 nyr_unit_mixed_value(&mixed) == 123 &&
                 (nyr_unit_mixed_set_code(&mixed, 29), mixed.code == 29) &&
                 nyr_unit_array_field_access(&array_fields) == 13 &&
                 array_fields.bytes[2] == 3 && array_fields.words[1] == 10 &&
                 nyr_unit_nested_global.items[1].values[0] == 29 &&
                 nyr_unit_float_layout_global.tag == 65 &&
                 nyr_unit_float_layout_global.values[0] == 0.0f &&
                 nyr_unit_float_layout_global.values[1] == 0.0f &&
                 nyr_unit_float_layout_global.code == 23 &&
                 nyr_unit_float_entries[1].name_len == 5 &&
                 nyr_unit_float_entries[1].name[0] == 'm' &&
                 nyr_unit_float_entries[1].scale == 2592000.0f &&
                 nyr_unit_opaque_identity(opaque) == opaque &&
                 nyr_unit_null_opaque_pointer() && nyr_unit_null_macro() &&
                 nyr_unit_auto_char_array() == 'A' + 'Z' &&
                 nyr_unit_pointer_difference(bytes) == 3 &&
                 nyr_unit_parenthesized_member_address(&page, 2) &&
                 nyr_unit_prefix_aggregate_decrement(&page) == 2 &&
                 page.index == 2 &&
                 nyr_unit_call_result_member_assignment(53) == 53 &&
                 nyr_unit_integer_to_double(-1234567L) == -1234567.0 &&
                 nyr_unit_aggregate_parameter(
                     3, (struct nyr_unit_token){"A", 6}) == 74 &&
                 nyr_unit_tag_decl_only_size() == sizeof(int) &&
                 nyr_unit_aggregate_array_designators() == 27 &&
                 nyr_unit_extern_array_definition() == 59 &&
                 nyr_unit_address_extent_matches() &&
                 nyr_unit_global_aggregate_pointer_path() == 73 &&
                 nyr_unit_int128_mul_add_shift(123, 3, 42, 4) == 25 &&
                 nyr_unit_int128_mul_u64_shift(0x100000002UL, 0x200000003UL,
                                               32) == 8589934599UL &&
                 nyr_unit_global_pointer_holders[1].dev[2] ==
                     &nyr_unit_global_pointer_value &&
                 nyr_unit_signed_bytes(-128) && nyr_unit_signed_bytes(-1) &&
                 nyr_unit_signed_bytes(127) && nyr_unit_signed_bytes(255) &&
                 nyr_unit_scoped_local_classes(0, 2.5, &scoped_value) == 26 &&
                 nyr_unit_scoped_local_classes(1, 2.5, &scoped_value) == 31 &&
                 nyr_unit_char_pointer(bytes) == 286 && bytes[1] == 'Z'
             ? 0
             : 1;
}
