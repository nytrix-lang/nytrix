/*
 * Fixture: nested aggregate compound literals and constant expressions.
 */
struct nyc_compound_value {
  unsigned long value;
};
#define NYC_IS_CONSTEXPR(x)                                             \
  (sizeof(int) == sizeof(*(8 ? ((void *)((long)(x) * 0l)) : (int *)8)))
#define NYC_CONST_TRUE(x) __builtin_choose_expr(NYC_IS_CONSTEXPR(x), (x), 0)
#define NYC_BUILD_BUG_ON_ZERO(e) ((int)sizeof(struct { int : (-!!(e)); }))
#define NYC_BUILD_BUG_ON_ZERO_STATIC(e)               \
  ((int)sizeof(struct { _Static_assert(!(e), ""); }))
unsigned long nyc_compound_literal(unsigned long value) {
  return ((struct nyc_compound_value){value}).value;
}
struct nyc_compound_inner {
  unsigned long value;
};
struct nyc_compound_outer {
  struct nyc_compound_inner inner;
};
unsigned long nyc_nested_aggregate_copy(unsigned long value) {
  struct nyc_compound_inner source = {value};
  struct nyc_compound_outer copy = {.inner = (source)};
  return copy.inner.value;
}
unsigned long nyc_kernel_constexpr_probe(unsigned long high,
                                         unsigned long low) {
  return NYC_BUILD_BUG_ON_ZERO(NYC_CONST_TRUE((low) > (high))) +
         NYC_BUILD_BUG_ON_ZERO_STATIC(NYC_CONST_TRUE((low) > (high))) + high;
}
unsigned long nyc_object_size_mode0(void *pointer) {
  return __builtin_object_size(pointer, 0);
}
unsigned long nyc_object_size_mode1(void *pointer) {
  return __builtin_object_size(pointer, 1);
}
unsigned long nyc_object_size_mode2(void *pointer) {
  return __builtin_object_size(pointer, 2);
}
unsigned long nyc_object_size_mode3(void *pointer) {
  return __builtin_object_size(pointer, 3);
}
unsigned long nyc_alignof_type(void) { return __alignof__(unsigned long); }
extern const unsigned long nyc_ulong_matrix[2][1];
const unsigned long *nyc_ulong_matrix_row(unsigned long row) {
  return nyc_ulong_matrix[row];
}
unsigned long nyc_ulong_array_read(const unsigned long *values) {
  return values[1];
}
unsigned long nyc_ulong_array_decay(void) {
  const unsigned long values[2] = {3, 37};
  return nyc_ulong_array_read(values);
}
void nyc_void_statement_expression(void) {
  ({
    (void)0;
    ;
  });
}
unsigned long nyc_asm_memory_cc(void) {
  unsigned long value = 43;
  __asm__ volatile("nop" ::: "memory", "cc");
  return value;
}
unsigned long nyc_block_static_assert(void) {
  return ({
    _Static_assert(sizeof(unsigned long) == 8, "word size");
    43UL;
  });
}
unsigned long nyc_typeof_shift_size(void) {
  __typeof__((unsigned int)(1 << 6)) value = 43;
  return sizeof(value);
}
struct nyc_seqcount {
  unsigned value;
};
struct nyc_seqlock {
  struct nyc_seqcount seqcount;
};
const struct nyc_seqcount *nyc_seqprop(const struct nyc_seqcount *seq) {
  return seq;
}
const struct nyc_seqcount *nyc_seqcheck(const struct nyc_seqcount *seq) {
  return seq;
}
unsigned long nyc_generic_seqcount(const struct nyc_seqlock *lock) {
  return nyc_seqcheck(_Generic(*(&lock->seqcount),
                          struct nyc_seqcount: nyc_seqprop)(&lock->seqcount)) ==
         &lock->seqcount;
}
unsigned long nyc_generic_const_pointer(const struct nyc_seqcount *pointer) {
  return _Generic(pointer,
      const struct nyc_seqcount *: 1UL,
      struct nyc_seqcount *: 0UL);
}
unsigned long nyc_generic_mutable_pointer(struct nyc_seqcount *pointer) {
  return _Generic(pointer,
      const struct nyc_seqcount *: 0UL,
      struct nyc_seqcount *: 1UL);
}
unsigned long
nyc_generic_pointer_typeof_depth(const struct nyc_seqcount *pointer) {
  __typeof__(_Generic(pointer,
                 const struct nyc_seqcount *: pointer,
                 default: (const struct nyc_seqcount *)0)) *slot = &pointer;
  return *slot == pointer;
}
struct nyc_seqcount **nyc_address_deref(struct nyc_seqcount **pointer) {
  return &*pointer;
}
struct nyc_static_key {
  unsigned long enabled;
};
struct nyc_static_key_false {
  struct nyc_static_key key;
};
struct nyc_tracepoint {
  struct nyc_static_key_false key;
};
extern struct nyc_tracepoint nyc_tracepoint;
struct nyc_static_key *nyc_tracepoint_key(void) {
  return &(&(nyc_tracepoint).key)->key;
}
void *nyc_return_address_level0(void) { return __builtin_return_address(0); }
unsigned long nyc_unreachable_branch(unsigned long value) {
  if (value)
    return value;
  __builtin_unreachable();
  return 0;
}
unsigned long nyc_for_comma_condition(unsigned long limit) {
  unsigned long value = 0;
  for (; (value++), value < limit;) {
  }
  return value;
}
struct nyc_cpumask {
  unsigned long bits[1];
};
typedef struct nyc_cpumask nyc_cpumask_var_t[1];
extern nyc_cpumask_var_t nyc_cpumask_global;
struct nyc_cpumask *nyc_cpumask_global_decay(void) {
  return *(__typeof__(*((&nyc_cpumask_global))) *)((&nyc_cpumask_global));
}
void nyc_cpumask_clear(struct nyc_cpumask *mask) { mask->bits[0] = 0; }
void nyc_cpumask_array_decay(nyc_cpumask_var_t *mask) {
  nyc_cpumask_clear(*mask);
}
void nyc_cpumask_array_param(nyc_cpumask_var_t mask) {
  nyc_cpumask_clear(mask);
}
