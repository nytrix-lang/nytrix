struct nyc_ref;
typedef void nyc_ref_callback(struct nyc_ref *);
struct nyc_ref_data { nyc_ref_callback *release; int count; };
struct nyc_ref { struct nyc_ref_data *data; };
struct nyc_list { int value; };
struct nyc_rlimit { unsigned long cur; };
struct nyc_signal { struct nyc_rlimit rlim[1]; };
struct nyc_task { struct nyc_signal *signal; };
struct nyc_rb_node { int value; };
struct nyc_latch_node { struct nyc_rb_node node[2]; };
struct nyc_empty { };
typedef struct nyc_empty *nyc_empty_row[3];
static nyc_empty_row nyc_empty_rows[2];
struct nyc_empty *nyc_array_pointer_element(unsigned long outer, unsigned long inner) {
  return nyc_empty_rows[outer][inner];
}
int nyc_builtin_memcpy_test(void) {
  char source[2] = {'x', 0};
  char destination[2] = {0, 0};
  __builtin_memcpy(destination, source, 2);
  return destination[0] != 'x' || destination[1] != 0 || __builtin_strlen(destination) != 1;
}
int nyc_local_static_string_pointer_array(void) {
  static const char *const values[] = {"kernel", "build"};
  return values[0][0] != 'k' || values[1][0] != 'b';
}
typedef _Bool nyc_test_bool;
int nyc_pointer_bool_initializer(void) {
  int value = 1;
  nyc_test_bool nonnull = &value;
  nyc_test_bool null = 0;
  return !nonnull || null;
}
_Bool nyc_pointer_bool_return(void *value) { return value; }
unsigned long nyc_sizeof_empty_deref(struct nyc_empty *value) {
  return sizeof(*value);
}
unsigned long nyc_typeof_choose_expr(struct nyc_empty *value) {
  typeof(__builtin_choose_expr(1, (unsigned char)0, (unsigned long)0)) chosen = 0;
  return sizeof(chosen);
}
static unsigned long nyc_array_compound_sum(const unsigned long *values) { return values[0] + values[1]; }
unsigned long nyc_array_compound_literal(void) {
  return nyc_array_compound_sum((const unsigned long[]){3, 4});
}
struct nyc_typedef_row_item { unsigned long value; };
typedef struct nyc_typedef_row_item nyc_typedef_row[1];
struct nyc_typedef_row_owner { nyc_typedef_row rows; };
static unsigned long nyc_typedef_row_sum(struct nyc_typedef_row_item *rows) { return rows[0].value; }
unsigned long nyc_typedef_row_field_decay(void) {
  struct nyc_typedef_row_owner owner;
  owner.rows[0].value = 13;
  return nyc_typedef_row_sum(owner.rows);
}
typedef struct nyc_cleanup_value { int *target; int amount; } nyc_cleanup_value;
static int nyc_cleanup_calls;
static void nyc_cleanup_apply(nyc_cleanup_value *value) {
  *value->target += value->amount;
  ++nyc_cleanup_calls;
}
static void nyc_cleanup_order(nyc_cleanup_value *value) {
  *value->target = *value->target * 10 + value->amount;
}
void nyc_cleanup_reverse(int *target) {
  nyc_cleanup_value first __attribute__((cleanup(nyc_cleanup_order))) = {target, 1};
  nyc_cleanup_value second __attribute__((cleanup(nyc_cleanup_order))) = {target, 2};
}
void nyc_cleanup_loop(int *target) {
  for (int i = 0; i < 3; ++i) {
    nyc_cleanup_value value __attribute__((cleanup(nyc_cleanup_apply))) = {target, 1};
    if (i == 0) continue;
    break;
  }
}
unsigned long nyc_typeof_deref_comma(unsigned long *bitmap) {
  return ({
    const typeof(*(bitmap)) mask__ = 1, bits__ = 2;
    typeof(*(bitmap)) old__, new__;
    old__ = *bitmap;
    new__ = (old__ & ~mask__) | bits__;
    *bitmap = new__;
    old__;
  });
}
int nyc_cleanup_return(int *target) {
  nyc_cleanup_value value __attribute__((cleanup(nyc_cleanup_apply))) = {target, 7};
  return 5;
}
void nyc_cleanup_block(int *target) {
  {
    nyc_cleanup_value value __attribute__((cleanup(nyc_cleanup_apply))) = {target, 3};
  }
}
static struct nyc_empty nyc_empty_object;
struct nyc_array_holder { char bytes[4]; };
static struct nyc_array_holder nyc_array_holder;
struct nyc_pointer_array_item { unsigned long value; };
struct nyc_pointer_array_owner { struct nyc_pointer_array_item item; };
static struct nyc_pointer_array_owner nyc_pointer_array_owner_a;
static struct nyc_pointer_array_owner nyc_pointer_array_owner_b;
static struct nyc_pointer_array_item *nyc_pointer_array[] = {
  &nyc_pointer_array_owner_a.item, &nyc_pointer_array_owner_b.item
};
char nyc_external_chars[2] = "xy";
void nyc_release(struct nyc_ref *ref) { ref->data->count = 42; }
static unsigned long nyc_release_address = (unsigned long)nyc_release;
static unsigned long nyc_release_address_value(void) {
  return nyc_release_address;
}
void nyc_release_forward(struct nyc_ref *ref) { return nyc_release(ref); }
void nyc_call_callback(nyc_ref_callback *callback, struct nyc_ref *ref) { callback(ref); }
struct nyc_array_holder *nyc_get_array_holder(void) {
  return &nyc_array_holder;
}
char *nyc_address_array_member(void) {
  return (char *)&nyc_get_array_holder()->bytes;
}
int nyc_extern_char_array(void) {
  extern char nyc_external_chars[];
  return nyc_external_chars[0];
}
int nyc_parenthesized_for_declarator(void) {
  int result = 0;
  for (int (index) = 0; index < 1; ++index) result += index + 1;
  return result;
}
void nyc_void_control_statement_expression(void) {
  ({ do { do { } while (0); } while (0); });
}
void nyc_trailing_asm_clobber(void) {
  asm volatile("" ::: "memory",);
}
int nyc_asm_flag_output(int *value) {
  unsigned char zero;
  asm volatile("decl %[value]" : [value] "+m"(*value), "=@cce"(zero) : : "memory",);
  return zero;
}
void nyc_prefetch_memory_input(const char *value) {
  asm volatile("prefetcht0 %1" : : "i"(0), "m"(*value));
}
int nyc_function_null(nyc_ref_callback *callback) {
  return callback == (void *)0;
}
int nyc_ref_alignment(struct nyc_ref *ref) { return __alignof__(ref->data); }
int nyc_typeof_postfix(struct nyc_ref *ref) {
  __typeof__(*(ref->data++)) *typed = ref->data;
  return typed == ref->data;
}
int nyc_typeof_call(struct nyc_ref *ref) {
  __typeof__(nyc_ref_alignment(ref)) alignment = 8;
  return alignment == 8;
}
int nyc_typeof_statement_expression(int value) {
  __typeof__(({ do { } while (0); value; })) typed = value;
  return typed == value;
}
int nyc_typeof_pointer_statement_expression(struct nyc_list *next) {
  _Static_assert(__builtin_types_compatible_p(
    __typeof__(*({ do { } while (0); (*(const volatile __typeof__(_Generic((next), default: next)) *)&next); })),
    struct nyc_list), "statement-expression pointer type");
  return next != 0;
}
unsigned long *nyc_generic_statement_expression(unsigned long *value) {
  return ({ do { } while (0); (*(const volatile __typeof__(_Generic((value), unsigned long *: value, default: value)) *)&value); });
}
unsigned long nyc_generic_field(unsigned limit, struct nyc_task *task) {
  return ({ do { } while (0); (*(const volatile __typeof__(_Generic((task->signal->rlim[limit].cur), unsigned long: task->signal->rlim[limit].cur, default: task->signal->rlim[limit].cur)) *)&(task->signal->rlim[limit].cur)); });
}
int nyc_typeof_aggregate_array(struct nyc_rb_node *node, int idx) {
  _Static_assert(__builtin_types_compatible_p(typeof(*(node)), typeof(((struct nyc_latch_node *)0)->node[idx])), "aggregate array element type");
  return idx;
}
unsigned long nyc_offsetof_aggregate_array(int idx) {
  return __builtin_offsetof(struct nyc_latch_node, node[idx]);
}
void *nyc_void_slot(void **slots) {
  return slots[0];
}
void *nyc_void_typed_slot(void **slots) {
  __typeof__(*(slots[0])) *typed = slots[0];
  return typed;
}
int nyc_gnu_omitted_conditional(int value) { return value ?: -7; }
int main(void) {
  struct nyc_ref_data data = { nyc_release_forward, 0 };
  struct nyc_ref ref = { &data };
  struct nyc_signal signal;
  struct nyc_task task = { &signal };
  void *slots[1];
  unsigned long generic_value = 23;
  int decremented_to_zero = 1;
  int decremented_nonzero = 2;
  int cleanup_target = 0;
  unsigned long typeof_bitmap = 5;
  signal.rlim[0].cur = 29;
  slots[0] = &data;
  nyc_call_callback(ref.data->release, &ref);
  nyc_void_control_statement_expression();
  nyc_trailing_asm_clobber();
  if (nyc_cleanup_return(&cleanup_target) != 5 || cleanup_target != 7 || nyc_cleanup_calls != 1) return 1;
  nyc_cleanup_block(&cleanup_target);
  if (cleanup_target != 10 || nyc_cleanup_calls != 2) return 1;
  cleanup_target = 0;
  nyc_cleanup_reverse(&cleanup_target);
  if (cleanup_target != 21) return 1;
  cleanup_target = 0;
  nyc_cleanup_loop(&cleanup_target);
  if (cleanup_target != 2 || nyc_cleanup_calls != 4) return 1;
  if (nyc_typeof_deref_comma(&typeof_bitmap) != 5 || typeof_bitmap != 6) return 1;
  if (!nyc_asm_flag_output(&decremented_to_zero) || decremented_to_zero != 0 || nyc_asm_flag_output(&decremented_nonzero) || decremented_nonzero != 1) return 1;
  nyc_prefetch_memory_input("x");
  return data.count != 42 || nyc_ref_alignment(&ref) != 8 || nyc_sizeof_empty_deref(&nyc_empty_object) != 0 || nyc_typeof_choose_expr(&nyc_empty_object) != 1 || nyc_array_compound_literal() != 7 || nyc_typedef_row_field_decay() != 13 || nyc_array_pointer_element(1, 2) != 0 || nyc_builtin_memcpy_test() || nyc_release_address_value() != (unsigned long)nyc_release || nyc_local_static_string_pointer_array() || nyc_pointer_bool_initializer() || !nyc_pointer_bool_return((void *)1) || nyc_pointer_bool_return(0) || nyc_address_array_member() != nyc_array_holder.bytes || nyc_pointer_array[0] != &nyc_pointer_array_owner_a.item || nyc_pointer_array[1] != &nyc_pointer_array_owner_b.item || nyc_extern_char_array() != 'x' || nyc_parenthesized_for_declarator() != 1 || !nyc_function_null(0) || nyc_function_null(nyc_release) || !nyc_typeof_postfix(&ref) || !nyc_typeof_call(&ref) || nyc_void_slot(slots) != &data || nyc_void_typed_slot(slots) != &data || nyc_gnu_omitted_conditional(3) != 3 || nyc_gnu_omitted_conditional(0) != -7 || !nyc_typeof_statement_expression(9) || !nyc_typeof_pointer_statement_expression((struct nyc_list *)1) || nyc_generic_statement_expression(&generic_value) != &generic_value || nyc_generic_field(0, &task) != 29 || nyc_offsetof_aggregate_array(1) != sizeof(struct nyc_rb_node) || &nyc_empty_object == 0;
}
