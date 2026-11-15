/*
 * C parser fixture for function attributes on an inline function definition.
 */
static inline int __attribute__((__warn_unused_result__))
nyc_must_check(int value) {
  return value;
}
extern char *nyc_malloc_attribute(void) __attribute__((__malloc__));
extern void *nyc_malloc_deallocator_attribute(void)
    __attribute__((malloc(free, 1)));
extern void *nyc_malloc_deallocator_alias_attribute(void)
    __attribute__((__malloc__(free, 1)));
extern void *nyc_alloc_size(unsigned long) __attribute__((__alloc_size__(1)));
extern int nyc_nonnull_attribute(const char *) __attribute__((__nonnull__(1)));
extern int nyc_access_attribute(void *, unsigned long)
    __attribute__((__access__(__write_only__, 1, 2)));
_Static_assert(__STDC__ == 1, "standard C macro");
__extension__ typedef __signed__ long long nyc_extended_i64;
nyc_extended_i64 nyc_extended_value;
extern unsigned long volatile
    __attribute__((aligned(64), section(".nyc_test"))) nyc_mid_attribute;
extern int nyc_redeclared;
extern int nyc_redeclared;
enum nyc_later_enum;
extern enum nyc_later_enum nyc_later_enum_fn(enum nyc_later_enum value);
enum nyc_later_enum {
  NYC_LATER_ZERO = 1 ? 10 : 20,
  NYC_LATER_ONE = 0 ? 2 : 12
};
_Static_assert(NYC_LATER_ZERO == 10 && NYC_LATER_ONE == 12,
               "enum conditional expression");
typedef void nyc_callback_type(int);
typedef int(nyc_cpu_distance_type)(unsigned int from, unsigned int to);
extern int nyc_fixed_array_parameter(unsigned int regs[8]);
typedef nyc_callback_type *nyc_callback_pointer;
extern nyc_callback_pointer nyc_callback;
struct nyc_segment_attribute_type {
  int value;
};
extern struct nyc_segment_attribute_type __seg_gs
    __attribute__((no_instrument_function)) *
    nyc_segment_attribute_pointer;
struct nyc_counted_array_attribute {
  unsigned long count;
  unsigned char data[] __attribute__((__counted_by__(count)));
};
unsigned long nyc_read_once_typeof_unqual(unsigned long *addr) {
  return (*(const volatile __typeof_unqual__(*(unsigned long *)addr) *)&(
      *(unsigned long *)addr));
}
void nyc_leading_local_declaration_attribute(void) {
  __attribute__((__unused__)) extern void nyc_local_attribute_target(void)
      __attribute__((__error__("diagnostic-only")));
}
unsigned long nyc_int128_low_cast(unsigned __int128 value) {
  return (unsigned long)value;
}
struct nyc_write_once_typeof_member {
  unsigned long value;
};
extern struct nyc_write_once_typeof_member nyc_write_once_global;
void nyc_write_once_typeof_member(void) {
  *(volatile typeof(nyc_write_once_global.value) *)&nyc_write_once_global
       .value = 17;
}
unsigned long nyc_nested_typeof_unqual(void) {
  return (typeof(__typeof_unqual__(
      nyc_write_once_global.value)))nyc_write_once_global.value;
}
int nyc_double_underscore_fallthrough(int value) {
  switch (value) {
  case 0:
    value++;
    __attribute__((__fallthrough__));
  case 1:
    return value;
  default:
    return -1;
  }
}
extern void nyc_no_sanitize(void) __attribute__((__no_sanitize_address__));
extern void *nyc_alloc_align(unsigned long) __attribute__((__alloc_align__(1)));
extern void nyc_deprecated_attribute(void)
    __attribute__((__deprecated__("test message")));
extern void nyc_returns_twice_attribute(void)
    __attribute__((__returns_twice__));
extern int nyc_direct_function_parameter(int callback(void *), void *context);
extern int (*nyc_global_callback)(int);
int nyc_call_global_callback(int value) { return nyc_global_callback(value); }
enum nyc_later_object_enum;
extern enum nyc_later_object_enum nyc_later_object;
enum nyc_later_object_enum { NYC_LATER_OBJECT_VALUE };
struct nyc_typeof_callback {
  int (*callback)(int);
};
extern struct nyc_typeof_callback *nyc_typeof_callback_object;
extern typeof(*nyc_typeof_callback_object->callback) nyc_typeof_callback_clone;
int nyc_call_typeof_callback_clone(void) {
  return nyc_typeof_callback_clone(3);
}
struct nyc_sample {
  unsigned int time, value;
};
struct nyc_samples {
  struct nyc_sample sample[3];
};
void nyc_chained_aggregate_array_assign(struct nyc_samples *owner,
                                        struct nyc_sample value) {
  owner->sample[2] = owner->sample[1] = owner->sample[0] = value;
}
void nyc_builtin_prefetch(void *pointer) {
  __builtin_prefetch(pointer);
  __builtin_prefetch(pointer, 1, 3);
}
struct nyc_control_block {
  unsigned long portid;
};
struct nyc_buffer {
  char cb[48];
};
struct nyc_control_block *nyc_control_block_cast(struct nyc_buffer *buffer) {
  return (struct nyc_control_block *)&((buffer)->cb);
}
struct nyc_callback_ops {
  unsigned int (*call)(void *);
};
unsigned int nyc_typeof_callback_result(struct nyc_callback_ops *ops,
                                        void *context) {
  typeof(ops->call) callback = ops->call;
  typeof(callback) callback_again = callback;
  return 1U ? 1U : callback_again(context);
}
#define NYC_READ_ONCE(value)                                               \
  ({                                                                       \
    typeof(_Generic((value), default: value)) __val = (value);             \
    (*(const volatile typeof(_Generic((__val), default: __val)) *)&__val); \
  })
unsigned int
nyc_statement_expression_callback_result(struct nyc_callback_ops *ops,
                                         void *context) {
  typeof(NYC_READ_ONCE(ops->call)) callback = NYC_READ_ONCE(ops->call);
  return 1U ? 1U : callback(context);
}
int nyc_address_generic_member(struct nyc_callback_ops *ops) {
  return &_Generic(ops, struct nyc_callback_ops *: ops, default: ops)->call !=
         0;
}
struct nyc_generic_member {
  int field;
};
int *nyc_address_generic_aggregate_member(struct nyc_generic_member *value) {
  return &_Generic(value, struct nyc_generic_member *: value, default: value)
              ->field;
}
struct nyc_generic_outer {
  struct nyc_generic_member member;
};
int nyc_generic_postfix_type(struct nyc_generic_outer *value) {
  _Static_assert(
      __builtin_types_compatible_p(typeof(_Generic(value,
                                              struct nyc_generic_outer *: value,
                                              default: value)
                                              ->member),
                                   struct nyc_generic_member),
      "typeof generic member postfix type");
  return 0;
}
int nyc_aggregate_local_assignment(void) {
  struct nyc_generic_member left = {.field = 1};
  struct nyc_generic_member right = {.field = 2};
  left = right;
  return left.field;
}
struct nyc_empty_aggregate {};
int nyc_empty_aggregate_parameter(struct nyc_empty_aggregate empty, int value) {
  return value;
}
int nyc_empty_aggregate_call(void) {
  struct nyc_empty_aggregate empty;
  return nyc_empty_aggregate_parameter(empty, 9);
}
int nyc_builtin_memcmp(const void *left, const void *right,
                       unsigned long size) {
  return __builtin_memcmp(left, right, size);
}
#define NYC_GNU_OPTIONAL(first, ...) first, ##__VA_ARGS__
int nyc_gnu_empty_variadic(void) {
  int values[] = {NYC_GNU_OPTIONAL(3)};
  return values[0];
}
int nyc_nonvoid_fallthrough(void) {}
int nyc_comma_while_condition(int value) {
  while (value--, value > 0) {
  }
  return value;
}
struct nyc_increment_member {
  int value;
};
int nyc_incremented_aggregate_pointer(struct nyc_increment_member *pointer) {
  return (++pointer)->value;
}
extern short nyc_global_s16_array[4];
void nyc_global_s16_array_write(unsigned int index, short value) {
  nyc_global_s16_array[index] = value;
}
extern int nyc_global_s32_array[4];
int nyc_global_s32_array_assignment_expr(int value) {
  return (nyc_global_s32_array[0] = value);
}
static struct nyc_generic_outer *nyc_get_generic_outer(void) {
  return (struct nyc_generic_outer *)0;
}
struct nyc_generic_member *nyc_address_function_call_member(void) {
  return &nyc_get_generic_outer()->member;
}
int nyc_nested_generic_switch_defaults(void) {
  return ({
           switch (sizeof(int)) {
           case 4:
             1;
           default:
             2;
           }
           3;
         }) +
         ({
           switch (sizeof(long)) {
           case 8:
             1;
           default:
             2;
           }
           4;
         });
}
int nyc_nested_callback_parameter(void (**callback)(int)) {
  return callback != 0;
}
static int nyc_statement_expression_target(void) { return 17; }
int nyc_statement_expression_indirect_call(void) {
  return ({ (&nyc_statement_expression_target); })();
}
void nyc_empty_statement_expression(void) { ({}); }
extern int nyc_nested_pointer_sink(void *);
int nyc_nested_pointer_address(char **value) {
  return nyc_nested_pointer_sink(&value);
}
static const int *nyc_const_pointee_global;
void nyc_assign_const_pointee_global(const int *value) {
  nyc_const_pointee_global = value;
}
extern char *nyc_global_pointer_array[2];
char **nyc_global_pointer_array_element_address(void) {
  return &nyc_global_pointer_array[1];
}
void nyc_char_pointer_increment(char **value) { *value += 1; }
void nyc_triple_pointer_increment(void ***value) { (*value)++; }
struct nyc_float_increment {
  double cost;
};
double nyc_float_field_increment(struct nyc_float_increment *value) {
  return value->cost--;
}
int nyc_local_array_trailing_comma(void) {
  unsigned short values[] = {
      3,
      5,
  };
  return values[1];
}
double nyc_float_argument(double (*callback)(double), float value) {
  return callback(value);
}
double nyc_float_zero_initializer(void) {
  double value = 0;
  return value;
}
struct nyc_f32_field {
  float value;
};
void nyc_f32_field_store(struct nyc_f32_field *object) {
  object->value = 1;
  object->value += 0.5f;
}
void nyc_pointer_table_assign(int **table, int *values) {
  table[0] = values;
  table[0][1] = 7;
}
void nyc_float_casted_pointer_store(float *values, double value) {
  ((float *)values)[0] = value;
}
void nyc_for_initializer_call(void);
void nyc_for_call_init(void) {
  for (nyc_for_initializer_call(); 0; nyc_for_initializer_call()) {
  }
}
void nyc_pointer_compound_step(char **value) { *value += 2; }
struct nyc_nested_node {
  int value;
};
struct nyc_nested_owner {
  struct nyc_nested_node **items;
};
int *nyc_nested_member_address(struct nyc_nested_owner *owner, int index) {
  return &owner->items[index]->value;
}
struct nyc_node_array_owner {
  struct nyc_nested_node *items[2];
};
void nyc_nested_pointer_array_store(struct nyc_node_array_owner *owner,
                                    int value) {
  owner->items[0]->value = value;
}
int *nyc_pointer_parameter_member_address(struct nyc_nested_node **items,
                                          int index) {
  return &items[index]->value;
}
void nyc_pointer_parameter_member_store(struct nyc_nested_node **items,
                                        int index, int value) {
  items[index]->value = value;
}
int nyc_local_tag_one(void) {
  struct nyc_local_tag {
    int value;
  };
  return (int)sizeof(struct nyc_local_tag);
}
int nyc_local_tag_two(void) {
  struct nyc_local_tag {
    long first;
    long second;
  };
  return (int)sizeof(struct nyc_local_tag);
}
struct nyc_enum_pair {
  const char *name;
  int value;
};
int nyc_local_aggregate_array(void) {
  const struct nyc_enum_pair values[] = {
      {"none", 1},
      {"full", 0},
      {"columns", 2},
      {0, 0},
  };
  return values[2].value;
}
struct nyc_stmt_sizeof;
struct nyc_stmt_array_owner {
  struct nyc_stmt_sizeof *items[40];
};
int nyc_member_array_element_size(struct nyc_stmt_array_owner *owner) {
  return (int)(sizeof(owner->items) / sizeof(owner->items[0]));
}
unsigned long
nyc_unparenthesized_member_array_size(struct nyc_stmt_array_owner *owner) {
  return sizeof owner->items;
}
int nyc_builtin_isnan(double value) { return __builtin_isnan(value); }
double nyc_builtin_infinity(void) { return __builtin_inf(); }
double nyc_float_pointer_store(void) {
  float source = 1.5f;
  double result = 0.0;
  double *target = &result;
  *target = source;
  return result;
}
extern void nyc_nothrow_attribute(void) __attribute__((__nothrow__));
extern void nyc_leaf_attribute(void) __attribute__((__leaf__));
extern void nyc_cold_attribute(void) __attribute__((__cold__));
extern void nyc_hot_attribute(void) __attribute__((hot));
typedef void nyc_void_typedef;
struct nyc_void_pointer_member {
  nyc_void_typedef *lock;
};
void nyc_void_pointer_member_store(struct nyc_void_pointer_member *member,
                                   void *value) {
  member->lock = value;
}
void nyc_void_pointer_indirect_store(void **slot, void *value) {
  *slot = value;
}
typedef float nyc_float_typedef;
extern int nyc_wchar_parameter(wchar_t);
extern unsigned int nyc_wint_parameter(wint_t);
extern long
nyc_redirect_declaration(const char *) __asm__("nyc_redirect_target")
    __attribute__((__nothrow__, __leaf__));
