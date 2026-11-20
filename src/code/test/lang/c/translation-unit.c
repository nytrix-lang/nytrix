/*
 * Translation-unit fixture: exercise standard headers and C library calls.
 */
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

struct nyr_unit_extra_semicolon {
  int value;
  ;
};
struct nyr_unit_redeclared_element;
typedef struct nyr_unit_redeclared_element nyr_unit_redeclared_element_t;
extern nyr_unit_redeclared_element_t *nyr_unit_redeclared_array[];
extern struct nyr_unit_redeclared_element *nyr_unit_redeclared_array[];
struct nyr_unit_redeclared_element {
  int value;
};
struct nyr_unit_redeclared_element nyr_unit_redeclared_value = {59};
struct nyr_unit_redeclared_element *nyr_unit_redeclared_array[] = {
    &nyr_unit_redeclared_value};
int nyr_unit_extern_array_definition(void) {
  return nyr_unit_redeclared_array[0]->value;
}
struct nyr_unit_global_pointer_item {
  int value;
};
struct nyr_unit_global_pointer_holder {
  struct nyr_unit_global_pointer_item *dev[4];
};
struct nyr_unit_global_pointer_holder nyr_unit_global_pointer_holders[2];
struct nyr_unit_global_pointer_holder *nyr_unit_global_pointer_base =
    nyr_unit_global_pointer_holders;
struct nyr_unit_global_pointer_item nyr_unit_global_pointer_value = {73};
int nyr_unit_global_aggregate_pointer_path(void) {
  nyr_unit_global_pointer_base[1].dev[2] = &nyr_unit_global_pointer_value;
  return nyr_unit_global_pointer_base[1].dev[2]->value;
}
typedef unsigned char nyr_unit_thunk_t[32];
extern nyr_unit_thunk_t nyr_unit_thunk_array[2];

extern int printf(const char *, ...);
extern void nyr_unit_compile_time_error_attribute(void)
    __attribute__((error("invalid call")));

long nyr_unit_helper(long value) { return value * 3; }
long nyr_unit_entry(long value) { return nyr_unit_helper(value) + 1; }
struct nyr_unit_array_decay_inner {
  char *name;
};
struct nyr_unit_array_decay_holder {
  struct nyr_unit_array_decay_inner entries[2];
};
char *nyr_unit_array_decay_assign(struct nyr_unit_array_decay_holder *holder,
                                  char *name) {
  holder->entries->name = name;
  return holder->entries->name;
}
static void nyr_unit_void_macro_sink(int *value, int delta) { *value += delta; }
#define NYC_VOID_CALL_IF(condition, call)                                      \
  if (condition)                                                               \
  (call)
int nyr_unit_parenthesized_void_macro(void) {
  int value = 0;
  NYC_VOID_CALL_IF(1, nyr_unit_void_macro_sink(&value, 23));
  return value;
}
#undef NYC_VOID_CALL_IF
int nyr_unit_atomic_slot;
int nyr_unit_block_extern_read(void) {
  extern int nyr_unit_block_extern_data;
  return nyr_unit_block_extern_data;
}
int nyr_unit_block_extern_add(int value) {
  extern int nyr_unit_block_extern_data;
  nyr_unit_block_extern_data += value;
  return nyr_unit_block_extern_data;
}
int nyr_unit_unsigned_pointer_order(void) {
  const char *text = "x";
  const char *limit = (const char *)(-1);
  return text < limit;
}
static int nyr_unit_write_double(double *value) {
  *value = 1234.0;
  return 1;
}
static int nyr_unit_accept_double_and_pointer(double value, long *result) {
  *result = (long)value;
  return 1;
}
int nyr_unit_address_taken_double_call(void) {
  double value = 2.5;
  long result = 0;
  return nyr_unit_write_double(&value) &&
         nyr_unit_accept_double_and_pointer(value, &result) && result == 1234;
}
static double nyr_unit_double_identity(double value) { return value; }
static int nyr_unit_capture_double_result(double value, long *result) {
  *result = (long)value;
  return 1;
}
int nyr_unit_indirect_double_result_call(void) {
  double (*identity)(double) = nyr_unit_double_identity;
  long result = 0;
  return nyr_unit_capture_double_result(identity(4321.0), &result) &&
         result == 4321;
}
static int nyr_unit_return_minus_one(void) { return -1; }
int nyr_unit_indirect_i32_negative_result(void) {
  int (*get_value)(void) = nyr_unit_return_minus_one;
  return get_value() == -1;
}
int nyr_unit_atomic_roundtrip(int value) {
  __atomic_store_n(&nyr_unit_atomic_slot, value, __ATOMIC_RELAXED);
  return __atomic_load_n(&nyr_unit_atomic_slot, __ATOMIC_RELAXED);
}
void nyr_unit_full_memory_barrier(void) { __sync_synchronize(); }
int nyr_unit_fallthrough_attribute(int value) {
  int result = 0;
  switch (value) {
  case 1:
    result = 1;
    __attribute__((fallthrough));
  case 2:
    result += 2;
    break;
  }
  return result;
}
unsigned short nyr_unit_bswap16(unsigned short value) {
  return __builtin_bswap16(value);
}
int nyr_unit_clzll(unsigned long long value) {
  int clzll = __builtin_clzll(value);
  int clzl = __builtin_clzl((unsigned long)value);
  int clz = __builtin_clz((unsigned int)value);
  unsigned long low_bit = (unsigned long)value & -(unsigned long)value;
  int ctzl = __builtin_ctzl((unsigned long)value);
  int expected_ctzl = 63 - __builtin_clzll(low_bit);
  return clzll == clzl && clz == __builtin_clzll((unsigned int)value) - 32 &&
                 ctzl == expected_ctzl
             ? clzll
             : -1;
}
int nyr_unit_add_overflow(long long left, long long right, long long *result) {
  return __builtin_add_overflow(left, right, result);
}
int nyr_unit_sub_overflow(long long left, long long right, long long *result) {
  return __builtin_sub_overflow(left, right, result);
}
int nyr_unit_mul_overflow(long long left, long long right, long long *result) {
  return __builtin_mul_overflow(left, right, result);
}
int nyr_unit_static_function_null(void) {
  static const struct {
    void (*callback)(void *);
  } item = {(void (*)(void *))0};
  return item.callback == 0;
}
static void nyr_unit_exit_callback(void) {}
int nyr_unit_atexit_register(void) { return atexit(nyr_unit_exit_callback); }
extern int nyr_unit_variadic_sink(const char *format, ...);
int nyr_unit_variadic_probe(void) { return nyr_unit_variadic_sink("%f", 1.25); }
int nyr_unit_stdio(int value) {
  return printf("C stdio: %d %.2f\n", value, 1.25);
}
struct nyr_unit_initialized_bits {
  unsigned char marker;
  unsigned int flag : 3;
  signed int delta : 5;
  void *address;
  void *scaled;
};
static struct nyr_unit_initialized_bits nyr_unit_bit_constants = {
    0xa5, 6, -3, (void *)&((char *)0)[7], (void *)&((long *)0)[3]};

int nyr_unit_bitfield_constants(void) {
  return nyr_unit_bit_constants.marker == 0xa5 &&
         nyr_unit_bit_constants.flag == 6 &&
         nyr_unit_bit_constants.delta == -3 &&
         nyr_unit_bit_constants.address == (void *)7 &&
         nyr_unit_bit_constants.scaled == (void *)24;
}

long nyr_unit_indexed_pointer_constant(long value) {
  void *pointer = (void *)&((char *)0)[value];
  return (long)pointer;
}

long nyr_unit_signed_short(short value) { return value; }

long nyr_unit_signed_short_load(short *value) { return *value; }

long nyr_unit_signed_short_narrow(long value) { return (short)value; }

long nyr_unit_if_else_local_pointer(int choose) {
  long left = 41;
  long right = 42;
  long *selected;
  if (choose) {
    selected = &left;
  } else {
    selected = &right;
  }
  return *selected;
}
pid_t nyr_unit_pid_identity(pid_t process_id) { return process_id; }

extern int nyr_unit_variadic_decl(int fixed, ...);
extern int nyr_unit_callback_decl(int (*)(void *, int), void *);
extern int nyr_unit_void_callback_decl(void (*)(void *), void *);
void (*nyr_unit_function_pointer_return(void))(void);

typedef int nyr_unit_i32;
typedef unsigned long long nyr_unit_u64;

nyr_unit_i32 nyr_unit_int_identity(nyr_unit_i32 value) { return value; }

int nyr_unit_int_add(int left, int right) {
  int result = left + right;
  result += 3;
  return result;
}

int nyr_unit_hex_e_integer(void) { return 0xe0; }

double nyr_unit_long_double_suffix(void) { return 1.25L; }

long nyr_unit_int_promote(int value) { return value; }

int nyr_unit_int_narrow(long value) { return value; }

long nyr_unit_int_pointer_roundtrip(int *value) {
  unsigned int sign_bits = 0x80000000U;
  long signed_alias = *(int *)&sign_bits;
  if (signed_alias >= 16384)
    return 0;
  long prior = *value;
  *value = (int)(prior + 4);
  return prior;
}

int nyr_unit_int_pointer_index(int *values) {
  values[1] = values[0] + 5;
  return values[1];
}

typedef struct nyr_unit_sizeof_node nyr_unit_sizeof_node;
struct nyr_unit_sizeof_node {
  long values[7];
};
struct nyr_unit_sizeof_array_owner {
  nyr_unit_sizeof_node **items;
};

unsigned long nyr_unit_sizeof_pointer_array_element(
    struct nyr_unit_sizeof_array_owner *owner) {
  return sizeof(owner->items[0]);
}

struct nyr_unit_f64_pointer_owner {
  double *values;
};

double
nyr_unit_f64_pointer_field_element(struct nyr_unit_f64_pointer_owner *owner) {
  return owner->values[0];
}

struct nyr_unit_u32_pointer_owner {
  unsigned int *values;
};

unsigned int
nyr_unit_u32_pointer_field_element(struct nyr_unit_u32_pointer_owner *owner,
                                   int index) {
  return owner->values[index];
}

struct nyr_unit_u16_pointer_owner {
  unsigned short *values;
};

unsigned short *nyr_unit_u16_pointer_field_element_address(
    struct nyr_unit_u16_pointer_owner *owner, int index) {
  return &owner->values[index];
}

unsigned short nyr_unit_atomic_u16_load(const unsigned short *value) {
  return __atomic_load_n(value, 0);
}

void nyr_unit_atomic_u16_store(unsigned short *address, unsigned short value) {
  __atomic_store_n(address, value, 0);
}

short nyr_unit_atomic_i16_load(const short *value) {
  return __atomic_load_n(value, 0);
}

struct nyr_unit_pointer_increment_record {
  unsigned long values[5];
};

int nyr_unit_aggregate_pointer_postincrement(void) {
  struct nyr_unit_pointer_increment_record records[2];
  struct nyr_unit_pointer_increment_record *current = records;
  struct nyr_unit_pointer_increment_record **cursor = &current;
  (*cursor)++;
  return current == records + 1;
}

struct nyr_unit_builtin_offsetof_owner {
  char prefix;
  long value;
};

unsigned long nyr_unit_builtin_offsetof_expression(void) {
  return __builtin_offsetof(struct nyr_unit_builtin_offsetof_owner, value);
}

struct nyr_unit_global_pointer_target {
  int value;
};
static struct nyr_unit_global_pointer_target nyr_unit_global_pointer_target = {
    42};
static struct nyr_unit_global_pointer_target *nyr_unit_global_pointer_table[2];

static void
nyr_unit_global_pointer_store(struct nyr_unit_global_pointer_target *table[],
                              int index,
                              struct nyr_unit_global_pointer_target *value) {
  table[index] = value;
}

static struct nyr_unit_global_pointer_target *
nyr_unit_global_pointer_load(struct nyr_unit_global_pointer_target *table[],
                             int index) {
  return table[index];
}

int nyr_unit_global_aggregate_pointer_array(void) {
  nyr_unit_global_pointer_store(nyr_unit_global_pointer_table, 1,
                                &nyr_unit_global_pointer_target);
  return nyr_unit_global_pointer_load(nyr_unit_global_pointer_table, 1)->value;
}

struct nyr_unit_array_callback {
  int (*identify)(int);
};
static struct nyr_unit_array_callback nyr_unit_array_callback_value;
static struct nyr_unit_array_callback *nyr_unit_array_callbacks[1];

static int nyr_unit_array_callback_identify(int value) { return value + 19; }

int nyr_unit_global_aggregate_pointer_array_call(int value) {
  nyr_unit_array_callback_value.identify = nyr_unit_array_callback_identify;
  nyr_unit_array_callbacks[0] = &nyr_unit_array_callback_value;
  if (nyr_unit_array_callbacks[0] && nyr_unit_array_callbacks[0]->identify)
    return nyr_unit_array_callbacks[0]->identify(value);
  return 0;
}

int nyr_unit_typeof_statement_lvalue_growth(int *value) {
  typeof(({
    *value;
    *value;
    *value;
    *value;
    *value;
    *value;
    *value;
    *value;
    *value;
    *value;
    *value;
    *value;
    *value;
    *value;
    *value;
    *value;
    *value;
    *value;
    *value;
    *value;
    7;
  })) result = 7;
  return result;
}

int nyr_unit_string_search(const char *text) {
  const char *match = strstr(text, "CONFIG_");
  return match != 0 && match[-1] == ' ';
}

int nyr_unit_config_scan(const char *p) {
  const char *q, *r;
  const char *start = p;
  int matches = 0;
  while ((p = strstr(p, "CONFIG_"))) {
    if (p > start && (isalnum(p[-1]) || p[-1] == '_')) {
      p += 7;
      continue;
    }
    p += 7;
    q = p;
    while (isalnum(*q) || *q == '_')
      ++q;
    size_t suffix_length = strlen("_MODULE");
    r = (size_t)(q - p) >= suffix_length &&
                !memcmp(q - suffix_length, "_MODULE", suffix_length)
            ? q - suffix_length
            : q;
    if (r > p)
      ++matches;
    p = q;
  }
  return matches;
}

extern void nyr_unit_config_record(int matches);

static void nyr_unit_void_config_scan(const char *p) {
  const char *q, *r;
  const char *start = p;
  int matches = 0;
  while ((p = strstr(p, "CONFIG_"))) {
    if (p > start && (isalnum(p[-1]) || p[-1] == '_')) {
      p += 7;
      continue;
    }
    p += 7;
    q = p;
    while (isalnum(*q) || *q == '_')
      ++q;
    size_t suffix_length = strlen("_MODULE");
    r = (size_t)(q - p) >= suffix_length &&
                !memcmp(q - suffix_length, "_MODULE", suffix_length)
            ? q - suffix_length
            : q;
    if (r > p)
      ++matches;
    p = r;
  }
  nyr_unit_config_record(matches);
}

int nyr_unit_void_config_scan_result(void) {
  static const char input[] = "/* CONFIG_A prefix CONFIG_B and _CONFIG_C */";
  char *buffer = malloc(sizeof(input));
  if (!buffer)
    return 0;
  for (size_t i = 0; i < sizeof(input); ++i)
    buffer[i] = input[i];
  nyr_unit_void_config_scan(buffer);
  for (size_t i = 0; i < sizeof(input); ++i) {
    if (buffer[i] != input[i]) {
      free(buffer);
      return 0;
    }
  }
  free(buffer);
  return 1;
}

int nyr_unit_opaque_pointer_address(void) {
  void *pointer = 0;
  void **slot = &pointer;
  *slot = 0;
  return pointer == 0;
}

struct nyr_unit_const_pointee {
  int value;
};

int nyr_unit_const_aggregate_pointer(void) {
  struct nyr_unit_const_pointee object = {47};
  const struct nyr_unit_const_pointee *view;
  view = &object;
  return view->value;
}

int nyr_unit_static_string_array(void) {
  static const char ordinal[] = "thstndrd";
  return ordinal[0] == 't' && ordinal[8] == 0 ? sizeof(ordinal) : 0;
}

long nyr_unit_int_loop(int count) {
  int sum = 0;
  for (int i = 0; i < count; ++i)
    sum += i;
  return sum;
}

#define NYC_EXPORT
#define NYC_PUBLIC NYC_EXPORT
#define NYC_VALUE (19 + 20)
#define NYC_VALUE_ALIAS NYC_VALUE
long NYC_PUBLIC nyr_unit_object_macro(long value) {
  return value + NYC_VALUE_ALIAS;
}

#define nyr_unit_self_macro nyr_unit_self_macro
long nyr_unit_self_macro(void) { return 7; }

int nyr_unit_char_pointer(char *data) {
  int first = data[0];
  data[1] = 90;
  return first + *(data + 2) + data[1];
}

int nyr_unit_string_literal_index(void) { return "aZ"[1]; }

int nyr_unit_unsigned_compare(nyr_unit_u64 left, nyr_unit_u64 right) {
  return left > right;
}

nyr_unit_u64 nyr_unit_unsigned_add(nyr_unit_u64 left, nyr_unit_u64 right) {
  return left + right;
}

nyr_unit_u64 nyr_unit_unsigned_div(nyr_unit_u64 value, nyr_unit_u64 divisor) {
  return value / divisor;
}

nyr_unit_u64 nyr_unit_unsigned_mod(nyr_unit_u64 value, nyr_unit_u64 divisor) {
  return value % divisor;
}

unsigned int nyr_unit_unsigned_int_div(unsigned int value,
                                       unsigned int divisor) {
  return value / divisor;
}

unsigned int nyr_unit_unsigned_int_mod(unsigned int value,
                                       unsigned int divisor) {
  return value % divisor;
}

nyr_unit_u64 nyr_unit_unsigned_shift(nyr_unit_u64 value, unsigned int count) {
  return value >> count;
}

nyr_unit_u64 nyr_unit_unsigned_max(void) { return 18446744073709551615ULL; }

int nyr_unit_for_comma_updates(void) {
  int value = 0;
  int count = 0;
  for (value++; value < 3; value++, count++) {
  }
  return value + count;
}

int nyr_unit_for_comma_initializer(void) {
  int value = 0;
  int count = 0;
  for (value = 1, count = 2; value < 4; value++, count++) {
  }
  return value + count;
}

unsigned int nyr_unit_u32_add(unsigned int left, unsigned int right) {
  return left + right;
}

int nyr_unit_u32_compare(unsigned int left, unsigned int right) {
  return left > right;
}

struct nyr_unit_u32_adjacent {
  unsigned int count;
  void *pointer;
};

unsigned int nyr_unit_u32_field_increment(struct nyr_unit_u32_adjacent *value) {
  return value->count++;
}

unsigned char nyr_unit_u8_increment(unsigned char value) { return value + 1; }
char nyr_unit_char_identity(char value) { return value; }

typedef int (*nyr_unit_callback)(int);
nyr_unit_callback nyr_unit_callback_identity(nyr_unit_callback callback) {
  return callback;
}

int nyr_unit_callback_parameter(int (*callback)(int), int value) {
  return callback(value);
}

int nyr_unit_discard_callback_result(int (*callback)(int), int value) {
  (void)callback(value);
  return 1;
}

char **nyr_unit_pointer_pointer_identity(char **value) { return value; }
char **nyr_unit_pointer_pointer_predecrement(char **value) { return --value; }
char **nyr_unit_pointer_pointer_postdecrement(char **value) {
  value--;
  return value;
}
int nyr_unit_return_with_unreachable_statement(int value) {
  return value;
  (void)value;
}
int nyr_unit_return_with_unreachable_label(int value) {
  if (value == 0)
    goto after_return;
  return value;
  (void)value;
after_return:
  return 17;
}
char ***nyr_unit_pointer_triple_identity(char ***value) { return value; }
int nyr_unit_string_pointer_array_index(char **values) { return values[1][1]; }
int nyr_unit_automatic_char_string(void) {
  char escaped[3] = "\\?";
  return escaped[0] == '\\' && escaped[1] == '?' && escaped[2] == '\0';
}
int nyr_unit_local_enum_scope(int condition) {
  if (condition) {
    enum nyr_unit_local_help { NYR_UNIT_HELP_A = 3, NYR_UNIT_HELP_B = 4 };
    return NYR_UNIT_HELP_A + NYR_UNIT_HELP_B;
  } else {
    enum nyr_unit_local_help { NYR_UNIT_HELP_A = 9, NYR_UNIT_HELP_B = 8 };
    return NYR_UNIT_HELP_A + NYR_UNIT_HELP_B;
  }
}

struct nyr_unit_mixed {
  int code;
  long value;
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

int nyr_unit_conditional_aggregate_pointer(
    int condition, struct nyr_unit_pointed_field *left,
    struct nyr_unit_pointed_field *right) {
  return (condition ? left : right)->value;
}

struct nyr_unit_pointer_to_aggregate_field {
  struct nyr_unit_pointed_field *items;
};
struct nyr_unit_aggregate_array_field {
  struct nyr_unit_pointed_field items[2];
};

int nyr_unit_aggregate_array_element_address(void) {
  struct nyr_unit_aggregate_array_field list;
  struct nyr_unit_pointed_field *item = &list.items[1];
  item->value = 42;
  return list.items[1].value;
}

int nyr_unit_auto_pointer_array(void) {
  const char *items[] = {"one", "two"};
  return items[0][0] + items[1][0];
}

static void nyr_unit_void_comma_side_effect(void) {}

int nyr_unit_void_comma_expression(void) {
  return (nyr_unit_void_comma_side_effect(), 7);
}

int nyr_unit_static_pointer_array(void) {
  static const char *const items[] = {"B-tree", "hash", "R-tree"};
  return items[0][0] + items[2][0];
}

struct nyr_unit_aggregate_pointer_slot {
  struct nyr_unit_aggregate_pointer_slot *next;
};
struct nyr_unit_char_pointer_slots {
  char **items;
};
struct nyr_unit_single_char_array {
  char value[1];
};

char *
nyr_unit_single_char_array_decay(struct nyr_unit_single_char_array *value) {
  return value->value;
}

void nyr_unit_set_callback_field(struct nyr_unit_callback_field *state,
                                 void (*callback)(void)) {
  (*(&state->callback) = callback);
}

void nyr_unit_call_callback_field(struct nyr_unit_callback_field *state) {
  state->callback();
}

int nyr_unit_callback_pointer_array_equal(
    struct nyr_unit_callback_pointer_array *state, void (*expected)(void)) {
  void (**items)(void);
  items = state->items;
  return items[0] == expected;
}

void nyr_unit_set_pointed_field(
    struct nyr_unit_pointer_to_aggregate_field *state, int value) {
  state->items[1].value = value;
}

void nyr_unit_set_aggregate_pointer_slot(
    struct nyr_unit_aggregate_pointer_slot *state,
    struct nyr_unit_aggregate_pointer_slot *value) {
  struct nyr_unit_aggregate_pointer_slot *p, **pp;
  p = value;
  pp = &state->next;
  *pp = p;
}

char *nyr_unit_char_pointer_slot(struct nyr_unit_char_pointer_slots *state,
                                 int index) {
  return *(&state->items[index]);
}

static struct nyr_unit_inline_array {
  int value;
} nyr_unit_inline_array[2] = {{3}, {4}};

int nyr_unit_inline_aggregate_array(void) {
  return nyr_unit_inline_array[0].value + nyr_unit_inline_array[1].value;
}

int nyr_unit_volatile_field_update(struct nyr_unit_volatile_field *state) {
  int previous = state->value;
  state->value = previous;
  return state->value;
}

double nyr_unit_volatile_pointer_and_local(volatile double *values) {
  volatile double total = values[0] + values[1];
  return total;
}

double nyr_unit_volatile_parameter(volatile double value) {
  value = value + 1.0;
  return value;
}

int nyr_unit_mixed_code(struct nyr_unit_mixed *value) { return value->code; }
long nyr_unit_mixed_value(struct nyr_unit_mixed *value) { return value->value; }
void nyr_unit_mixed_set_code(struct nyr_unit_mixed *value, int code) {
  value->code = code;
}

struct nyr_unit_array_fields {
  unsigned char bytes[4];
  unsigned int words[2];
};

unsigned int nyr_unit_array_field_access(struct nyr_unit_array_fields *value) {
  value->words[1] = 10U;
  return value->bytes[2] + value->words[1];
}

struct nyr_unit_nested_inner {
  long values[2];
};
struct nyr_unit_nested_outer {
  struct nyr_unit_nested_inner items[2];
};
struct nyr_unit_nested_outer nyr_unit_nested_global = {
    {{{11, 0}}, {{29, 0}}},
};
struct nyr_unit_float_layout {
  char tag;
  float values[2];
  int code;
};
struct nyr_unit_float_layout nyr_unit_float_layout_global = {'A', {0, 0}, 23};
struct nyr_unit_float_entry {
  unsigned char name_len;
  char name[7];
  float limit;
  float scale;
};
struct nyr_unit_float_entry nyr_unit_float_entries[] = {
    {3, "day", 5.373485e6, 86400.0},
    {5, "month", 176546.0, 30.0 * 86400.0},
};

const char nyr_unit_global_text[] = "abc";
int nyr_unit_global_text_at_one(void) { return nyr_unit_global_text[1]; }
struct nyr_unit_token {
  const char *text;
  unsigned int length;
};
int nyr_unit_aggregate_parameter(int seed, struct nyr_unit_token token) {
  return seed + (int)token.length + token.text[0];
}
struct nyr_unit_tag_decl_only {
  struct nyr_unit_token;
  int value;
};
int nyr_unit_tag_decl_only_size(void) {
  return sizeof(struct nyr_unit_tag_decl_only);
}
struct nyr_unit_array_initializer_element {
  int first;
  int second;
};
struct nyr_unit_array_initializer {
  struct nyr_unit_array_initializer_element extent[2];
  int tail;
};
int nyr_unit_aggregate_array_designators(void) {
  struct nyr_unit_array_initializer value = {
      .extent[0] = {.first = 4, .second = 7},
      .extent[1] = {.first = 9, .second = 2},
      .tail = 5,
  };
  return value.extent[0].first + value.extent[0].second +
         value.extent[1].first + value.extent[1].second + value.tail;
}
static const unsigned char nyr_unit_global_bytes[] = {0, 10, 255};
int nyr_unit_global_bytes_at_two(void) { return nyr_unit_global_bytes[2]; }
extern const char *const nyr_unit_extern_string_pointer_array[];
struct nyr_unit_extern_pointer_bounds_item;
extern const struct nyr_unit_extern_pointer_bounds_item
    *const nyr_unit_extern_pointer_bounds_start[],
        *const nyr_unit_extern_pointer_bounds_end[];
static const struct nyr_unit_extern_pointer_bounds_item
    *nyr_unit_empty_aggregate_pointer_array[2] = {};
static char nyr_unit_address_extent[16];
unsigned long nyr_unit_address_extent_end =
    (unsigned long)&nyr_unit_address_extent + sizeof(nyr_unit_address_extent);
int nyr_unit_address_extent_matches(void) {
  return nyr_unit_address_extent_end ==
         (unsigned long)&nyr_unit_address_extent[16];
}
const char *const nyr_unit_extern_string_pointer_array[] = {"alpha", "beta"};
int nyr_unit_extern_string_pointer_array_at_one(void) {
  return nyr_unit_extern_string_pointer_array[1][0];
}
static const unsigned char nyr_unit_global_stride_bytes[] = {17, 23, 42, 81};
int nyr_unit_global_stride_probe(unsigned int index) {
  return nyr_unit_global_stride_bytes[(index * 4 ^ index * 3 ^ 1) % 4];
}
static const char nyr_unit_global_char_bytes[] = {'a', 'b', 'c', 'd'};
int nyr_unit_global_char_bytes_at_two(void) {
  const char *bytes = nyr_unit_global_char_bytes;
  return bytes[2] + *(&nyr_unit_global_char_bytes[2]);
}
static const unsigned char *nyr_unit_global_bytes_at_one =
    &nyr_unit_global_bytes[1];
int nyr_unit_global_byte_pointer_value(void) {
  return nyr_unit_global_bytes_at_one[1];
}
static const char *const nyr_unit_string_pointer_table[] = {"alpha", "beta"};
const char *nyr_unit_string_pointer_table_at_one(void) {
  return nyr_unit_string_pointer_table[1];
}

int nyr_unit_global_i32 = -17;
unsigned int nyr_unit_global_u32 = 4294967295U;
unsigned char nyr_unit_global_u8 = 250;
long nyr_unit_global_i64 = 0;
double nyr_unit_global_f64 = 1.5;
char *nyr_unit_global_pointer = 0;
extern int nyr_unit_host_global;

int nyr_unit_global_i32_read(void) { return nyr_unit_global_i32; }
unsigned int nyr_unit_global_u32_read(void) { return nyr_unit_global_u32; }
unsigned char nyr_unit_global_u8_read(void) { return nyr_unit_global_u8; }
long nyr_unit_global_i64_read(void) { return nyr_unit_global_i64; }
double nyr_unit_global_f64_read(void) { return nyr_unit_global_f64; }
int nyr_unit_host_global_read(void) { return nyr_unit_host_global; }

void nyr_unit_global_write(void) {
  nyr_unit_global_i32 = 29;
  nyr_unit_global_u32 = 7;
  nyr_unit_global_u8 = 9;
  nyr_unit_global_i64 = 41;
  nyr_unit_global_f64 = 2.5;
  nyr_unit_host_global = 13;
}

struct nyr_unit_mutable_global {
  int value;
};
typedef struct nyr_unit_mutable_global nyr_unit_mutable_global_t;
typedef struct nyr_unit_alias_local {
  int value;
} nyr_unit_alias_local;

int nyr_unit_alias_local_value(void) {
  nyr_unit_alias_local state;
  state.value = 5;
  state.value += 7;
  return state.value;
}

struct nyr_unit_mutable_global nyr_unit_mutable_global = {7};

int nyr_unit_global_aggregate_update(void) {
  nyr_unit_mutable_global.value = nyr_unit_mutable_global.value + 1;
  return nyr_unit_mutable_global.value;
}

int nyr_unit_static_aggregate_update(void) {
  static struct nyr_unit_mutable_global state = {4};
  state.value = state.value + 1;
  return state.value;
}

nyr_unit_mutable_global_t *nyr_unit_static_aggregate_address(void) {
  static struct nyr_unit_mutable_global state = {73};
  return &state;
}

int nyr_unit_static_aggregate_array_count(void) {
  static struct nyr_unit_mutable_global states[3] = {{1}, {2}, {3}};
  return sizeof(states) / sizeof(states[0]);
}

extern int nyr_unit_host_variadic_probe(int tag, ...);

int nyr_unit_variadic_call(void) {
  return nyr_unit_host_variadic_probe(3, 2.5f, 7);
}

int nyr_unit_variadic_sum(int tag, ...) {
  va_list args;
  va_start(args, tag);
  int first = va_arg(args, int);
  double real = va_arg(args, double);
  long second = va_arg(args, long);
  int third = va_arg(args, int);
  int fourth = va_arg(args, int);
  int fifth = va_arg(args, int);
  int sixth = va_arg(args, int);
  va_end(args);
  if (real != 3.0)
    return -1;
  return (int)(tag + first + second + third + fourth + fifth + sixth);
}

static int nyr_unit_variadic_read(va_list args) { return va_arg(args, int); }

int nyr_unit_variadic_copy(int tag, ...) {
  va_list args;
  va_list copy;
  va_start(args, tag);
  va_copy(copy, args);
  int original = va_arg(args, int);
  int copied = nyr_unit_variadic_read(copy);
  va_end(copy);
  va_end(args);
  return tag + original + copied;
}

int nyr_unit_switch(int value) {
  int result = 0;
  switch (value) {
  case 1:
    result += 2;
  case 2:
    result += 3;
    break;
  case ';':
    result += 11;
    break;
  case '\f':
    result += 12;
    break;
  default:
    result += 7;
  }
  return result;
}

int nyr_unit_switch_range(int value) {
  switch (value) {
  case '0' ... '7':
    return 1;
  case '8':
    return 2;
  default:
    return 0;
  }
}

enum nyr_unit_switch_state {
  NYR_UNIT_SWITCH_DONE = 10,
  NYR_UNIT_SWITCH_LOCK = 20,
};

int nyr_unit_switch_enum_case(int value) {
  switch (value) {
  case NYR_UNIT_SWITCH_DONE:
    return 1;
  case NYR_UNIT_SWITCH_LOCK ... NYR_UNIT_SWITCH_LOCK + 2:
    return 2;
  default:
    return 0;
  }
}

static inline void nyr_unit_unreachable_asm_string_helper(long *value) {
  __asm__ volatile("incq %0" : "+r"(*value));
}

static inline int nyr_unit_unreachable_asm_goto(void) {
  return ({
    int value = 0;
    __asm__ goto("jmp %l[cc_label]" : : : : cc_label);
    if (0) {
    cc_label:
      value = 1;
    }
    value;
  });
}

#if defined(__GNUC__) && !defined(__clang__)
static inline __attribute__((__indirect_branch__("keep"))) int
nyr_unit_indirect_branch_attribute(void) {
  return 0;
}
#endif

struct nyr_unit_volatile_link {
  struct nyr_unit_volatile_link *next;
};
struct nyr_unit_array_address_probe {
  char bytes[4];
};
char *
nyr_unit_address_of_array_member(struct nyr_unit_array_address_probe *probe) {
  return (char *)&(probe->bytes);
}
int nyr_unit_choose_expr(int value) {
  return __builtin_choose_expr(__builtin_types_compatible_p(typeof(value), int),
                               value < 0 ? -value : value, 0);
}
extern struct nyr_unit_volatile_link *nyr_unit_pointer_matrix[3][2];
extern struct nyr_unit_volatile_link *nyr_unit_pointer_matrix_expected;

int nyr_unit_statement_expression_braced_if(int value) {
  const char *asm_name = "nyr_unit_unreachable_asm_string_helper";
  struct nyr_unit_volatile_link link = {0};
  *(volatile typeof(link.next) *)&(link.next) = &link;
  typeof((&(link.next)) + 0) next_slot = &link.next;
  if (link.next != &link || link.next != ((void *)&link + 0) ||
      ((void *)&link + 1) != (void *)((char *)&link + 1) ||
      next_slot != &link.next ||
      nyr_unit_pointer_matrix[2][1] != nyr_unit_pointer_matrix_expected ||
      !__builtin_types_compatible_p(typeof(link.next), typeof(&link))) {
    return -1;
  }
  return ({
    int result = 0;
    if (value) {
      result = 17;
    }
    result + (asm_name[0] - 'n');
  });
}

void *nyr_unit_tied_asm_pointer(void *value) {
  return ({
    value ? ({ __asm__ volatile("nop"); }) : ({ __asm__ volatile("nop"); });
    ({ __asm__ __inline volatile("nop" : : "i"(2)); });
    unsigned long pointer;
    __asm__("" : "=r"(pointer) : "0"((void *)value));
    (void *)(pointer + 0);
  });
}

static inline int __order_base_2(unsigned long value) {
  return value > 1 ? 64 - __builtin_clzl(value - 1) : 0;
}

int nyr_unit_kernel_order_base_immediate(void) {
  __asm__ volatile(
      "nop"
      :
      : "i"(
          (__builtin_constant_p(64)
               ? ((64) == 0 || (64) == 1
                      ? 0
                      : ((64) - 1 < 2 ? 0 : 63 - __builtin_clzll((64) - 1)) + 1)
               : __order_base_2(64))));
  return 6;
}

int nyr_unit_switch_nested_label(int value) {
  int result = 1;
  switch (value) {
  case 1: {
    result = 2;
  case 3:
    result += 4;
    break;
  }
  default:
    result = 7;
  }
  return result;
}

int nyr_unit_switch_returns(int value) {
  switch (value) {
  case 1:
    return 5;
  case 2:
    return 7;
  default:
    return 9;
  }
}

int nyr_unit_switch_goto_label(int value) {
  switch (value) {
  case 1:
    goto finish_case;
  finish_case:
    return 11;
  default:
    return 12;
  }
}

struct nyr_unit_typed_pointer_field {
  long *value;
};

long nyr_unit_typed_pointer_field_read(
    struct nyr_unit_typed_pointer_field *field) {
  return *field->value;
}

long nyr_unit_pointer_field_index_write(
    struct nyr_unit_typed_pointer_field *field, long value) {
  field->value[1] = value;
  return field->value[1];
}

int nyr_unit_chained_local_array_assignment(void) {
  int values[3];
  values[1] = values[2] = -1;
  return values[1] + values[2];
}

struct nyr_unit_opaque;
typedef struct nyr_unit_opaque nyr_unit_opaque;
nyr_unit_opaque *nyr_unit_opaque_identity(nyr_unit_opaque *value) {
  return value;
}

int nyr_unit_null_opaque_pointer(void) {
  nyr_unit_opaque *value = 0;
  return value == 0;
}

int nyr_unit_null_macro(void) {
  char *value = NULL;
  return value == NULL;
}

char *nyr_unit_static_null_pointer(void) {
  static char *value = NULL;
  return value;
}

int nyr_unit_auto_char_array(void) {
  char bytes[4];
  bytes[0] = 'A';
  bytes[3] = 'Z';
  return bytes[0] + bytes[3];
}

int nyr_unit_pointer_difference(char *bytes) { return (bytes + 3) - bytes; }

struct nyr_unit_addressed_bytes {
  unsigned char *bytes;
  int index;
};

struct nyr_unit_call_result_member {
  int value;
};

struct nyr_unit_aggregate_pointer_chain {
  struct nyr_unit_aggregate_pointer_chain **previous;
  struct nyr_unit_aggregate_pointer_chain *next;
};

static struct nyr_unit_call_result_member nyr_unit_call_result_member_state;

static struct nyr_unit_call_result_member *
nyr_unit_call_result_member_target(void) {
  return &nyr_unit_call_result_member_state;
}

int nyr_unit_call_result_member_assignment(int value) {
  nyr_unit_call_result_member_target()->value = value;
  return nyr_unit_call_result_member_target()->value;
}

void nyr_unit_aggregate_pointer_chain_assign(
    struct nyr_unit_aggregate_pointer_chain *node) {
  node->next->previous = node->previous;
}

int nyr_unit_conditional_comma(int condition, int *value) {
  return condition ? (*value = 7, 1) : 0;
}

int nyr_unit_parenthesized_member_address(struct nyr_unit_addressed_bytes *page,
                                          int index) {
  unsigned char *cell = &(page)->bytes[index];
  return cell == page->bytes + index && cell >= page->bytes;
}

int nyr_unit_prefix_aggregate_decrement(struct nyr_unit_addressed_bytes *page) {
  return --page->index;
}

double nyr_unit_integer_to_double(long value) { return (double)value; }

typedef signed char nyr_unit_i8;
static const nyr_unit_i8 nyr_unit_signed_byte_table[] = {-128, -1, 0, 127};

static signed char nyr_unit_signed_byte_identity(signed char value) {
  return value;
}

static int nyr_unit_signed_byte_at(const signed char *bytes, int index) {
  return bytes[index];
}

int nyr_unit_signed_bytes(int value) {
  signed char local = (signed char)value;
  signed char *pointer = &local;
  struct byte_fields {
    signed char value;
    unsigned char guard;
    signed char values[2];
  };
  struct byte_fields fields = {-7, 165, {-128, 127}};
  fields.value /= 2;
  *pointer /= 2;
  return nyr_unit_signed_byte_identity((signed char)value) ==
             (signed char)value &&
         local == (signed char)value / 2 && *pointer == local &&
         pointer[0] == local && fields.value == -3 && fields.guard == 165 &&
         fields.values[0] == -128 && fields.values[1] == 127 &&
         nyr_unit_signed_byte_table[0] == -128 &&
         nyr_unit_signed_byte_table[1] == -1 &&
         nyr_unit_signed_byte_at(nyr_unit_signed_byte_table, 3) == 127 &&
         nyr_unit_signed_byte_at(nyr_unit_signed_byte_table, 0) == -128;
}

static long nyr_unit_scoped_pointer_target(long *value, int increment) {
  return *value + increment;
}

long nyr_unit_scoped_local_classes(int branch, double input, long *pointer) {
  long result = 0;
  if (branch) {
    double temporary = input * 2.0;
    result = (long)temporary;
  }
  {
    long *temporary = pointer;
    result += nyr_unit_scoped_pointer_target(temporary, 3);
  }
  return result;
}

void *nyr_unit_allocation_header(int size) {
  long *allocation = malloc(size + 8);
  if (allocation) {
    allocation[0] = size;
    allocation++;
  }
  return allocation;
}

struct nyr_unit_link {
  struct nyr_unit_link *next;
  int value;
};

extern void nyr_unit_write_int(int *value);
extern void nyr_unit_fill_bytes(void *value, int count);
int nyr_unit_loop_storage(void) {
  int total = 0;
  for (int i = 0; i < 4096; ++i) {
    unsigned char bytes[4096];
    struct loop_pair {
      long first;
      long second;
    };
    struct loop_pair pair;
    pair.first = i;
    pair.second = i + 1;
    volatile int marker = i;
    nyr_unit_fill_bytes(&bytes, sizeof(bytes));
    if (pair.second - pair.first != 1 || marker != i)
      return -1;
    total += bytes[0] + bytes[4095];
  }
  return total;
}

int nyr_unit_whole_array_address(void) {
  unsigned char bytes[16];
  nyr_unit_fill_bytes(&bytes, sizeof(bytes));
  return bytes[0] == 165 && bytes[15] == 165;
}

int nyr_unit_out_parameter(void) {
  int value = -1;
  nyr_unit_write_int(&value);
  return value == 42;
}

unsigned char *nyr_unit_static_byte(void) {
  static unsigned char value = 17;
  return &value;
}

typedef struct nyr_unit_sized_record nyr_unit_sized_record;
struct nyr_unit_sized_record {
  char bytes[19];
  double scale;
  unsigned char tail;
};
struct nyr_unit_layout_descriptor {
  long byte_size;
  long short_size;
  long float_size;
  long record_size;
};
static const struct nyr_unit_layout_descriptor nyr_unit_layout = {
    sizeof(signed char), sizeof(short), sizeof(float),
    sizeof(nyr_unit_sized_record)};

int nyr_unit_global_sizeof(void) {
  return nyr_unit_layout.byte_size == 1 && nyr_unit_layout.short_size == 2 &&
         nyr_unit_layout.float_size == 4 &&
         nyr_unit_layout.record_size == sizeof(nyr_unit_sized_record) &&
         nyr_unit_layout.record_size == 40;
}

int nyr_unit_static_counter(void) {
  static int value = 40;
  return ++value;
}

int nyr_unit_static_link_value(struct nyr_unit_link *node) {
  static struct nyr_unit_link *saved;
  if (node)
    saved = node;
  return saved->value;
}

int nyr_unit_parameter_loop(struct nyr_unit_link *node) {
  while (1) {
    if (!node->next)
      return node->value;
    node = node->next;
    continue;
  }
}

int nyr_unit_short_lvalue_stores(void) {
  struct narrow_fields {
    unsigned short flags;
    unsigned short guard;
    short delta;
    unsigned short tail;
  };
  struct narrow_fields values = {2, 42330, -7, 21930};
  (values.flags) |= 1;
  (values.delta) /= 2;
  return values.flags == 3 && values.guard == 42330 && values.delta == -3 &&
         values.tail == 21930;
}

unsigned long nyr_unit_int128_mul_add_shift(unsigned long value,
                                            unsigned int multiplier,
                                            unsigned long addend,
                                            unsigned int shift) {
  return (unsigned long)((((unsigned __int128)value * multiplier) + addend) >>
                         shift);
}

unsigned long nyr_unit_int128_mul_u64_shift(unsigned long value,
                                            unsigned long multiplier,
                                            unsigned int shift) {
  return (unsigned long)(((unsigned __int128)value * multiplier) >> shift);
}

#if 0
extern "C" {
long nyr_unit_disabled(void) { return 0; }
}
#else
long nyr_unit_enabled_branch(void) { return 42; }
#endif
