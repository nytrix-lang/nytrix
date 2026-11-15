/*
 * Host fixture: provide aggregate objects and helpers for literal tests.
 */
struct nyc_compound_value {
  unsigned long value;
};
unsigned long nyc_compound_literal(unsigned long);
unsigned long nyc_nested_aggregate_copy(unsigned long);
unsigned long nyc_kernel_constexpr_probe(unsigned long, unsigned long);
unsigned long nyc_object_size_mode0(void *);
unsigned long nyc_object_size_mode1(void *);
unsigned long nyc_object_size_mode2(void *);
unsigned long nyc_object_size_mode3(void *);
unsigned long nyc_alignof_type(void);
unsigned long nyc_for_comma_condition(unsigned long);
unsigned long nyc_ulong_array_decay(void);
void nyc_void_statement_expression(void);
unsigned long nyc_asm_memory_cc(void);
unsigned long nyc_block_static_assert(void);
unsigned long nyc_typeof_shift_size(void);
struct nyc_seqcount {
  unsigned value;
};
struct nyc_seqlock {
  struct nyc_seqcount seqcount;
};
unsigned long nyc_generic_seqcount(const struct nyc_seqlock *);
unsigned long nyc_generic_const_pointer(const struct nyc_seqcount *);
unsigned long nyc_generic_mutable_pointer(struct nyc_seqcount *);
unsigned long nyc_generic_pointer_typeof_depth(const struct nyc_seqcount *);
struct nyc_seqcount **nyc_address_deref(struct nyc_seqcount **);
struct nyc_static_key {
  unsigned long enabled;
};
struct nyc_static_key_false {
  struct nyc_static_key key;
};
struct nyc_tracepoint {
  struct nyc_static_key_false key;
};
struct nyc_static_key *nyc_tracepoint_key(void);
void *nyc_return_address_level0(void);
unsigned long nyc_unreachable_branch(unsigned long);
const unsigned long nyc_ulong_matrix[2][1] = {{1}, {43}};
const unsigned long *nyc_ulong_matrix_row(unsigned long);
struct nyc_cpumask {
  unsigned long bits[1];
};
typedef struct nyc_cpumask nyc_cpumask_var_t[1];
struct nyc_cpumask nyc_cpumask_global[1] = {{{47}}};
struct nyc_tracepoint nyc_tracepoint = {{{43}}};
struct nyc_static_key *nyc_tracepoint_key(void);
struct nyc_cpumask *nyc_cpumask_global_decay(void);
void nyc_cpumask_array_decay(nyc_cpumask_var_t *);
void nyc_cpumask_array_param(nyc_cpumask_var_t);
int main(void) {
  struct nyc_cpumask mask[1] = {{{17}}};
  nyc_void_statement_expression();
  if (nyc_asm_memory_cc() != 43)
    return 1;
  if (nyc_block_static_assert() != 43)
    return 1;
  if (nyc_typeof_shift_size() != 4)
    return 1;
  struct nyc_seqlock seq = {{19}};
  if (!nyc_generic_seqcount(&seq))
    return 1;
  struct nyc_seqcount *seq_pointer = &seq.seqcount;
  if (nyc_generic_const_pointer(seq_pointer) != 1 ||
      nyc_generic_mutable_pointer(seq_pointer) != 1)
    return 1;
  if (nyc_generic_pointer_typeof_depth(seq_pointer) != 1)
    return 1;
  if (nyc_address_deref(&seq_pointer) != &seq_pointer)
    return 1;
  if (nyc_tracepoint_key() != &nyc_tracepoint.key.key ||
      nyc_tracepoint_key()->enabled != 43)
    return 1;
  if (!nyc_return_address_level0())
    return 1;
  if (nyc_unreachable_branch(43) != 43)
    return 1;
  nyc_cpumask_array_decay(&mask);
  if (mask[0].bits[0] != 0)
    return 1;
  mask[0].bits[0] = 19;
  nyc_cpumask_array_param(mask);
  return mask[0].bits[0] != 0 ||
         nyc_cpumask_global_decay() != nyc_cpumask_global ||
         nyc_cpumask_global_decay()->bits[0] != 47 ||
         nyc_ulong_matrix_row(1)[0] != 43 ||
         nyc_compound_literal(0x123456789abcdef0UL) != 0x123456789abcdef0UL ||
         nyc_nested_aggregate_copy(0x123456789abcdef0UL) !=
             0x123456789abcdef0UL ||
         nyc_kernel_constexpr_probe(7, 3) != 7 ||
         nyc_kernel_constexpr_probe(3, 7) != 3 ||
         nyc_object_size_mode0((void *)1) != ~0UL ||
         nyc_object_size_mode1((void *)1) != ~0UL ||
         nyc_object_size_mode2((void *)1) != 0 ||
         nyc_object_size_mode3((void *)1) != 0 || nyc_alignof_type() != 8 ||
         nyc_for_comma_condition(7) != 7 || nyc_ulong_array_decay() != 37;
}
