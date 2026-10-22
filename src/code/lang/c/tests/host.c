#include <stdio.h>
#include <string.h>

extern long c_kernel(long left, long right);
extern long c_reference(long left, long right);
extern long c_choose(long left, long right);
extern long c_choose_reference(long left, long right);
extern long c_predicate(long value);
extern long c_predicate_reference(long value);
extern long c_bitwise(long left, long right);
extern long c_bitwise_reference(long left, long right);
extern long c_shift_left(long value, long amount);
extern long c_shift_left_reference(long value, long amount);
extern long c_shift_right(long value, long amount);
extern long c_shift_right_reference(long value, long amount);
extern long c_shift_precedence(long value);
extern long c_shift_precedence_reference(long value);
extern long c_shift_additive_precedence(long value);
extern long c_shift_additive_precedence_reference(long value);
extern long c_shift_compound(long value);
extern long c_shift_compound_reference(long value);
extern long c_logic_and_skip(long *slot);
extern long c_logic_and_skip_reference(long *slot);
extern long c_logic_or_skip(long *slot);
extern long c_logic_or_skip_reference(long *slot);
extern long c_logic_and_run(long *slot);
extern long c_logic_and_run_reference(long *slot);
extern long c_logic_or_run(long *slot);
extern long c_logic_or_run_reference(long *slot);
extern long c_logic_and_precedence(void);
extern long c_logic_and_precedence_reference(void);
extern long c_logic_or_precedence(void);
extern long c_logic_or_precedence_reference(void);
extern long c_cond_true(long *slot);
extern long c_cond_true_reference(long *slot);
extern long c_cond_false(long *slot);
extern long c_cond_false_reference(long *slot);
extern long c_cond_precedence(long value);
extern long c_cond_precedence_reference(long value);
extern long c_cond_nested(long value);
extern long c_cond_nested_reference(long value);
extern long c_prefix_increment(long value);
extern long c_prefix_increment_reference(long value);
extern long c_prefix_decrement(long value);
extern long c_prefix_decrement_reference(long value);
extern long c_postfix_update(long value);
extern long c_postfix_update_reference(long value);
extern long c_postfix_decrement(long value);
extern long c_postfix_decrement_reference(long value);
extern long c_for_increment(long count);
extern long c_for_increment_reference(long count);
extern long c_while_increment(long count);
extern long c_while_increment_reference(long count);
extern long c_do_sum(long count);
extern long c_do_sum_reference(long count);
extern long c_do_break(long count);
extern long c_do_break_reference(long count);
extern long c_do_nested_conditional(long count);
extern long c_do_nested_conditional_reference(long count);
extern double c_double_postfix(double value);
extern double c_double_postfix_reference(double value);
extern long c_load(const long *pointer);
extern long c_load_reference(const long *pointer);
extern double c_f64_load(const double *pointer);
extern double c_f64_load_reference(const double *pointer);
extern long c_store(long *slot, long value);
extern long c_store_reference(long *slot, long value);
extern double c_f64_store(double *slot, double value);
extern double c_f64_store_reference(double *slot, double value);
extern long c_locals(long left, long right);
extern long c_locals_reference(long left, long right);
extern long c_many_locals(void);
extern long c_many_locals_reference(void);
extern long c_many_functions(void);
extern long c_many_functions_reference(void);
extern long c_target_link(void);
extern long c_target_link_reference(void);
extern long c_seven_params(long a, long b, long c, long d, long e, long f,
                           long g);
extern long c_seven_params_reference(long a, long b, long c, long d, long e,
                                     long f, long g);
extern long c_seven_call(long a, long b, long c, long d, long e, long f,
                         long g);
extern long c_seven_call_reference(long a, long b, long c, long d, long e,
                                   long f, long g);
extern long c_ten_params(long a, long b, long c, long d, long e, long f, long g,
                         long h, long i, long j);
extern long c_ten_params_reference(long a, long b, long c, long d, long e,
                                   long f, long g, long h, long i, long j);
extern long c_ten_call(long a, long b, long c, long d, long e, long f, long g,
                       long h, long i, long j);
extern long c_ten_call_reference(long a, long b, long c, long d, long e, long f,
                                 long g, long h, long i, long j);
extern double c_ten_doubles(double a, double b, double c, double d, double e,
                            double f, double g, double h, double i, double j);
extern double c_ten_doubles_reference(double a, double b, double c, double d,
                                      double e, double f, double g, double h,
                                      double i, double j);
extern double c_ten_double_call(double a, double b, double c, double d,
                                double e, double f, double g, double h,
                                double i, double j);
extern double c_ten_double_call_reference(double a, double b, double c,
                                          double d, double e, double f,
                                          double g, double h, double i,
                                          double j);
extern double c_mixed_abi(long i0, double d0, long i1, double d1, long i2,
                          double d2, long i3, double d3, long i4, double d4,
                          long i5, double d5, long i6, double d6, double d7,
                          double d8);
extern double c_mixed_abi_reference(long i0, double d0, long i1, double d1,
                                    long i2, double d2, long i3, double d3,
                                    long i4, double d4, long i5, double d5,
                                    long i6, double d6, double d7, double d8);
extern double c_mixed_abi_call(long i0, double d0, long i1, double d1, long i2,
                               double d2, long i3, double d3, long i4,
                               double d4, long i5, double d5, long i6,
                               double d6, double d7, double d8);
extern double c_mixed_abi_call_reference(long i0, double d0, long i1, double d1,
                                         long i2, double d2, long i3, double d3,
                                         long i4, double d4, long i5, double d5,
                                         long i6, double d6, double d7,
                                         double d8);
extern long c_recursive(long value);
extern long c_recursive_reference(long value);
extern long c_empty_statements(long count);
extern long c_empty_statements_reference(long count);
extern long c_local_update(long left, long right);
extern long c_local_update_reference(long left, long right);
extern long c_multi_declarator(long seed);
extern long c_multi_declarator_reference(long seed);
extern long c_declare_then_assign(long left, long right);
extern long c_declare_then_assign_reference(long left, long right);
extern long c_parameter_update(long value);
extern long c_parameter_update_reference(long value);
extern long c_call(long left, long right);
extern long c_call_reference(long left, long right);
extern long c_while_sum(long count);
extern long c_while_sum_reference(long count);
extern long c_while_early_return(long count);
extern long c_while_early_return_reference(long count);
extern long c_while_continue(long count);
extern long c_while_continue_reference(long count);
extern long c_while_break(long count);
extern long c_while_break_reference(long count);
extern long c_nested_while_control(long count);
extern long c_nested_while_control_reference(long count);
extern long c_for_sum(long count);
extern long c_for_sum_reference(long count);
extern long c_for_multi_declarators(long count);
extern long c_for_multi_declarators_reference(long count);
extern long c_for_early_return(long count);
extern long c_for_early_return_reference(long count);
extern long c_for_shadow(long seed);
extern long c_for_shadow_reference(long seed);
extern long c_nested_for_sum(long count);
extern long c_nested_for_sum_reference(long count);
extern long c_for_continue(long count);
extern long c_for_continue_reference(long count);
extern long c_for_break(long count);
extern long c_for_break_reference(long count);
extern long c_nested_for_control(long count);
extern long c_nested_for_control_reference(long count);
extern long c_for_assign_sum(long count);
extern long c_for_assign_sum_reference(long count);
extern long c_for_body_step(long count);
extern long c_for_body_step_reference(long count);
extern long c_index_load(const long *items, long index);
extern long c_index_load_reference(const long *items, long index);
extern long c_i64_array(const long items[], long index);
extern long c_i64_array_reference(const long items[], long index);
extern long c_i64_array_forward(const long items[], long index);
extern long c_i64_array_forward_reference(const long items[], long index);
extern long c_pointer_add(const long *items, long index);
extern long c_pointer_add_reference(const long *items, long index);
extern long c_pointer_add_reversed(const long *items, long index);
extern long c_pointer_add_reversed_reference(const long *items, long index);
extern long c_pointer_subtract(const long *items, long index);
extern long c_pointer_subtract_reference(const long *items, long index);
extern long c_pointer_compound_offset(long *items, long index);
extern long c_pointer_compound_offset_reference(long *items, long index);
extern long c_pointer_postfix_increment(long *items);
extern long c_pointer_postfix_increment_reference(long *items);
extern long c_pointer_prefix_increment(long *items);
extern long c_pointer_prefix_increment_reference(long *items);
extern long c_pointer_prefix_decrement(long *items);
extern long c_pointer_prefix_decrement_reference(long *items);
extern long c_const_data_pointer_increment(const long *items);
extern long c_const_data_pointer_increment_reference(const long *items);
extern long c_i64_array_store(long items[], long index, long value);
extern long c_i64_array_store_reference(long items[], long index, long value);
extern long c_index_store(long *items, long index, long value);
extern long c_index_store_reference(long *items, long index, long value);
extern double c_f64_index(const double *items, long index);
extern double c_f64_index_reference(const double *items, long index);
extern double c_f64_array(const double items[], long index);
extern double c_f64_array_reference(const double items[], long index);
extern double c_f64_array_forward(const double items[], long index);
extern double c_f64_array_forward_reference(const double items[], long index);
extern double c_double_pointer_add(const double *items, long index);
extern double c_double_pointer_add_reference(const double *items, long index);
extern double c_double_pointer_compound_offset(double *items, long index);
extern double c_double_pointer_compound_offset_reference(double *items,
                                                         long index);
extern double c_double_pointer_postfix_increment(double *items);
extern double c_double_pointer_postfix_increment_reference(double *items);
extern double c_f64_array_store(double items[], long index, double value);
extern double c_f64_array_store_reference(double items[], long index,
                                          double value);
extern double c_f64_index_store(double *items, long index, double value);
extern double c_f64_index_store_reference(double *items, long index,
                                          double value);
extern long c_if_increment(long value, long flag);
extern long c_if_increment_reference(long value, long flag);
extern long c_double_truth(double value);
extern long c_double_truth_reference(double value);
extern long c_if_early_return(long value);
extern long c_if_early_return_reference(long value);
extern long c_compound(long value);
extern long c_compound_reference(long value);
extern long c_compound_assignments(long value);
extern long c_compound_assignments_reference(long value);
extern double c_double_compound_assignment(double value);
extern double c_double_compound_assignment_reference(double value);
extern long c_pointer_compound_assignment(long *slot, long value);
extern long c_pointer_compound_assignment_reference(long *slot, long value);
extern long c_index_compound_assignment(long *items, long index, long value);
extern long c_index_compound_assignment_reference(long *items, long index,
                                                  long value);
extern double c_double_pointer_compound_assignment(double *slot, double value);
extern double c_double_pointer_compound_assignment_reference(double *slot,
                                                             double value);
extern long c_shadow_local(long value);
extern long c_shadow_local_reference(long value);
extern double c_double(double left, double right);
extern double c_double_reference(double left, double right);
extern double c_double_choose(double left, double right);
extern double c_double_choose_reference(double left, double right);
extern double c_double_local(double left, double right);
extern double c_double_local_reference(double left, double right);
extern double c_double_local_update(double left, double right);
extern double c_double_local_update_reference(double left, double right);
extern double c_double_call(double left, double right);
extern double c_double_call_reference(double left, double right);
extern void c_void_store(long *out, long value);
extern void c_void_store_reference(long *out, long value);
extern void c_void_store_return(long *out, long value);
extern void c_void_store_return_reference(long *out, long value);
extern long c_void_call(long *out, long value);
extern long c_void_call_reference(long *out, long value);
extern void *c_void_pointer_identity(void *pointer);
extern void *c_void_pointer_identity_reference(void *pointer);

typedef long (*c_logic_function_t)(long *slot);

static int check_logic_case(const char *name, c_logic_function_t compiled,
                            c_logic_function_t reference) {
  long compiled_slot = 0;
  long reference_slot = 0;
  long compiled_result = compiled(&compiled_slot);
  long reference_result = reference(&reference_slot);
  if (compiled_result == reference_result && compiled_slot == reference_slot)
    return 0;
  fprintf(stderr, "C logical %s mismatch: (%ld, %ld) != (%ld, %ld)\n", name,
          compiled_result, compiled_slot, reference_result, reference_slot);
  return 1;
}

/*
 * Aggregate ABI checks (structs.c).
 */
struct c_point {
  long x;
  long y;
};
struct c_big {
  long a;
  long b;
  long c;
  long d;
};
struct c_pair {
  double first;
  double second;
};
struct c_mixed {
  long tag;
  double value;
};
extern long c_point_sum(struct c_point p);
extern long c_point_sum_reference(struct c_point p);
extern struct c_point c_point_add(struct c_point a, struct c_point b);
extern struct c_point c_point_add_reference(struct c_point a, struct c_point b);
extern long c_big_sum(struct c_big b);
extern long c_big_sum_reference(struct c_big b);
extern struct c_big c_big_make(long seed);
extern struct c_big c_big_make_reference(long seed);
extern double c_pair_second(struct c_pair p);
extern double c_pair_second_reference(struct c_pair p);
extern struct c_pair c_pair_make(double base);
extern struct c_pair c_pair_make_reference(double base);
extern long c_mixed_blend(struct c_mixed m, long scale);
extern long c_mixed_blend_reference(struct c_mixed m, long scale);
extern long c_many_with_struct(long a, long b, long c, long d, long e,
                               struct c_point f, long g);
extern long c_many_with_struct_reference(long a, long b, long c, long d, long e,
                                         struct c_point f, long g);
extern long c_struct_array_roundtrip(long seed);
extern long c_struct_array_roundtrip_reference(long seed);

int main(void) {
  long opaque_value = 37;
  if (c_void_pointer_identity(&opaque_value) !=
      c_void_pointer_identity_reference(&opaque_value)) {
    fprintf(stderr, "C opaque pointer return ABI mismatch\n");
    return 1;
  }
  long compiled_void = 0;
  long reference_void = 0;
  c_void_store(&compiled_void, 17);
  c_void_store_reference(&reference_void, 17);
  if (compiled_void != reference_void) {
    fprintf(stderr, "C void implicit-return function mismatch\n");
    return 1;
  }
  compiled_void = 0;
  reference_void = 0;
  c_void_store_return(&compiled_void, 23);
  c_void_store_return_reference(&reference_void, 23);
  if (compiled_void != reference_void) {
    fprintf(stderr, "C void explicit-return function mismatch\n");
    return 1;
  }
  compiled_void = 0;
  reference_void = 0;
  long compiled_void_call = c_void_call(&compiled_void, 31);
  long reference_void_call = c_void_call_reference(&reference_void, 31);
  if (compiled_void_call != reference_void_call ||
      compiled_void != reference_void) {
    fprintf(stderr, "C void call-statement mismatch\n");
    return 1;
  }
  static const long shift_values[] = {0, 1, 3, 37, 1024};
  static const long shift_amounts[] = {0, 1, 3, 10};
  for (size_t i = 0; i < sizeof(shift_values) / sizeof(shift_values[0]); ++i) {
    for (size_t j = 0; j < sizeof(shift_amounts) / sizeof(shift_amounts[0]);
         ++j) {
      long value = shift_values[i];
      long amount = shift_amounts[j];
      if (c_shift_left(value, amount) !=
              c_shift_left_reference(value, amount) ||
          c_shift_right(value, amount) !=
              c_shift_right_reference(value, amount)) {
        fprintf(stderr, "C shift mismatch for (%ld, %ld)\n", value, amount);
        return 1;
      }
    }
    long value = shift_values[i];
    if (c_shift_precedence(value) != c_shift_precedence_reference(value) ||
        c_shift_additive_precedence(value) !=
            c_shift_additive_precedence_reference(value) ||
        c_shift_compound(value) != c_shift_compound_reference(value)) {
      fprintf(stderr, "C shift precedence/compound mismatch for %ld\n", value);
      return 1;
    }
  }
  if (check_logic_case("&& short-circuit", c_logic_and_skip,
                       c_logic_and_skip_reference) ||
      check_logic_case("|| short-circuit", c_logic_or_skip,
                       c_logic_or_skip_reference) ||
      check_logic_case("&& evaluation", c_logic_and_run,
                       c_logic_and_run_reference) ||
      check_logic_case("|| evaluation", c_logic_or_run,
                       c_logic_or_run_reference) ||
      check_logic_case("?: true arm", c_cond_true, c_cond_true_reference) ||
      check_logic_case("?: false arm", c_cond_false, c_cond_false_reference))
    return 1;
  if (c_logic_and_precedence() != c_logic_and_precedence_reference() ||
      c_logic_or_precedence() != c_logic_or_precedence_reference()) {
    fprintf(stderr, "C logical operator precedence mismatch\n");
    return 1;
  }
  static const long conditional_inputs[] = {-1, 0, 1, 20};
  for (size_t i = 0;
       i < sizeof(conditional_inputs) / sizeof(conditional_inputs[0]); ++i) {
    long value = conditional_inputs[i];
    if (c_cond_precedence(value) != c_cond_precedence_reference(value) ||
        c_cond_nested(value) != c_cond_nested_reference(value)) {
      fprintf(stderr, "C conditional expression mismatch for %ld\n", value);
      return 1;
    }
  }
  static const long increment_inputs[] = {-20, -1, 0, 1, 9, 40};
  for (size_t i = 0; i < sizeof(increment_inputs) / sizeof(increment_inputs[0]);
       ++i) {
    long value = increment_inputs[i];
    if (c_prefix_increment(value) != c_prefix_increment_reference(value) ||
        c_prefix_decrement(value) != c_prefix_decrement_reference(value) ||
        c_postfix_update(value) != c_postfix_update_reference(value) ||
        c_postfix_decrement(value) != c_postfix_decrement_reference(value)) {
      fprintf(stderr, "C increment/decrement mismatch for %ld\n", value);
      return 1;
    }
  }
  static const long inc_loop_counts[] = {0, 1, 2, 9, 40};
  for (size_t i = 0; i < sizeof(inc_loop_counts) / sizeof(inc_loop_counts[0]);
       ++i) {
    long count = inc_loop_counts[i];
    if (c_for_increment(count) != c_for_increment_reference(count) ||
        c_while_increment(count) != c_while_increment_reference(count) ||
        c_do_sum(count) != c_do_sum_reference(count) ||
        c_do_break(count) != c_do_break_reference(count) ||
        c_do_nested_conditional(count) !=
            c_do_nested_conditional_reference(count)) {
      fprintf(stderr, "C loop-control mismatch for %ld\n", count);
      return 1;
    }
  }
  static const double double_inputs[] = {-2.25, 0.0, 1.5, 40.25};
  for (size_t i = 0; i < sizeof(double_inputs) / sizeof(double_inputs[0]);
       ++i) {
    double value = double_inputs[i];
    double compiled = c_double_postfix(value);
    double reference = c_double_postfix_reference(value);
    if (compiled != reference) {
      fprintf(stderr, "C double postfix increment mismatch for %.17g\n", value);
      return 1;
    }
  }
  long compiled_slot = -23;
  long reference_slot = compiled_slot;
  long compiled_value = c_pointer_compound_assignment(&compiled_slot, 41);
  long reference_value =
      c_pointer_compound_assignment_reference(&reference_slot, 41);
  if (compiled_value != reference_value || compiled_slot != reference_slot) {
    fprintf(stderr, "C pointer compound-assignment mismatch\n");
    return 1;
  }
  long compiled_items[] = {5, 13, 27};
  long reference_items[] = {5, 13, 27};
  compiled_value = c_index_compound_assignment(compiled_items, 1, 42);
  reference_value =
      c_index_compound_assignment_reference(reference_items, 1, 42);
  if (compiled_value != reference_value ||
      memcmp(compiled_items, reference_items, sizeof(compiled_items)) != 0) {
    fprintf(stderr, "C indexed compound-assignment mismatch\n");
    return 1;
  }
  double compiled_double = 3.5;
  double reference_double = compiled_double;
  double compiled_double_result =
      c_double_pointer_compound_assignment(&compiled_double, -2.0);
  double reference_double_result =
      c_double_pointer_compound_assignment_reference(&reference_double, -2.0);
  if (compiled_double_result != reference_double_result ||
      compiled_double != reference_double) {
    fprintf(stderr, "C double pointer compound-assignment mismatch\n");
    return 1;
  }
  static const long compound_inputs[] = {-20, -1, 0, 1, 9, 40};
  for (size_t i = 0; i < sizeof(compound_inputs) / sizeof(compound_inputs[0]);
       ++i) {
    long value = compound_inputs[i];
    if (c_compound_assignments(value) !=
        c_compound_assignments_reference(value)) {
      fprintf(stderr, "C compound-assignment mismatch for %ld\n", value);
      return 1;
    }
  }
  for (size_t i = 0; i < sizeof(double_inputs) / sizeof(double_inputs[0]);
       ++i) {
    double value = double_inputs[i];
    if (c_double_compound_assignment(value) !=
        c_double_compound_assignment_reference(value)) {
      fprintf(stderr, "C double compound-assignment mismatch for %.17g\n",
              value);
      return 1;
    }
  }

  long many_locals = c_many_locals();
  long many_locals_reference = c_many_locals_reference();
  if (many_locals != many_locals_reference) {
    fprintf(stderr, "C many-locals mismatch: %ld != %ld\n", many_locals,
            many_locals_reference);
    return 1;
  }
  long many_functions = c_many_functions();
  long many_functions_reference = c_many_functions_reference();
  if (many_functions != many_functions_reference) {
    fprintf(stderr, "C many-functions mismatch: %ld != %ld\n", many_functions,
            many_functions_reference);
    return 1;
  }
  if (c_target_link() != c_target_link_reference()) {
    fprintf(stderr, "C target-link sample mismatch: %ld != %ld\n",
            c_target_link(), c_target_link_reference());
    return 1;
  }
  long seven_params = c_seven_params(1, 2, 3, 4, 5, 6, 7);
  long seven_params_reference = c_seven_params_reference(1, 2, 3, 4, 5, 6, 7);
  if (seven_params != seven_params_reference) {
    fprintf(stderr, "C seven-parameter mismatch: %ld != %ld\n", seven_params,
            seven_params_reference);
    return 1;
  }
  long seven_call = c_seven_call(1, 2, 3, 4, 5, 6, 7);
  long seven_call_reference = c_seven_call_reference(1, 2, 3, 4, 5, 6, 7);
  if (seven_call != seven_call_reference) {
    fprintf(stderr, "C seven-argument call mismatch: %ld != %ld\n", seven_call,
            seven_call_reference);
    return 1;
  }
  long ten_params = c_ten_params(1, 2, 3, 4, 5, 6, 7, 8, 9, 10);
  long ten_params_reference =
      c_ten_params_reference(1, 2, 3, 4, 5, 6, 7, 8, 9, 10);
  if (ten_params != ten_params_reference) {
    fprintf(stderr, "C ten-parameter mismatch: %ld != %ld\n", ten_params,
            ten_params_reference);
    return 1;
  }
  long ten_call = c_ten_call(1, 2, 3, 4, 5, 6, 7, 8, 9, 10);
  long ten_call_reference = c_ten_call_reference(1, 2, 3, 4, 5, 6, 7, 8, 9, 10);
  if (ten_call != ten_call_reference) {
    fprintf(stderr, "C ten-argument call mismatch: %ld != %ld\n", ten_call,
            ten_call_reference);
    return 1;
  }
  double ten_doubles =
      c_ten_doubles(0.5, 1.0, 1.5, 2.0, 2.5, 3.0, 3.5, 4.0, 4.5, 5.0);
  double ten_doubles_reference =
      c_ten_doubles_reference(0.5, 1.0, 1.5, 2.0, 2.5, 3.0, 3.5, 4.0, 4.5, 5.0);
  if (ten_doubles != ten_doubles_reference) {
    fprintf(stderr, "C ten-double parameter mismatch: %.17g != %.17g\n",
            ten_doubles, ten_doubles_reference);
    return 1;
  }
  double ten_double_call =
      c_ten_double_call(0.5, 1.0, 1.5, 2.0, 2.5, 3.0, 3.5, 4.0, 4.5, 5.0);
  double ten_double_call_reference = c_ten_double_call_reference(
      0.5, 1.0, 1.5, 2.0, 2.5, 3.0, 3.5, 4.0, 4.5, 5.0);
  if (ten_double_call != ten_double_call_reference) {
    fprintf(stderr, "C ten-double call mismatch: %.17g != %.17g\n",
            ten_double_call, ten_double_call_reference);
    return 1;
  }

  double mixed_abi = c_mixed_abi(1, 0.25, 2, 0.50, 3, 0.75, 4, 1.00, 5, 1.25, 6,
                                 1.50, 7, 1.75, 2.00, 2.25);
  double mixed_abi_reference =
      c_mixed_abi_reference(1, 0.25, 2, 0.50, 3, 0.75, 4, 1.00, 5, 1.25, 6,
                            1.50, 7, 1.75, 2.00, 2.25);
  if (mixed_abi != mixed_abi_reference) {
    fprintf(stderr, "C mixed integer/f64 parameter mismatch: %.17g != %.17g\n",
            mixed_abi, mixed_abi_reference);
    return 1;
  }
  double mixed_abi_call =
      c_mixed_abi_call(1, 0.25, 2, 0.50, 3, 0.75, 4, 1.00, 5, 1.25, 6, 1.50, 7,
                       1.75, 2.00, 2.25);
  double mixed_abi_call_reference =
      c_mixed_abi_call_reference(1, 0.25, 2, 0.50, 3, 0.75, 4, 1.00, 5, 1.25, 6,
                                 1.50, 7, 1.75, 2.00, 2.25);
  if (mixed_abi_call != mixed_abi_call_reference) {
    fprintf(stderr, "C mixed integer/f64 call mismatch: %.17g != %.17g\n",
            mixed_abi_call, mixed_abi_call_reference);
    return 1;
  }
  static const long recursive_inputs[] = {0, 1, 2, 5, 10, 15};
  for (size_t i = 0; i < sizeof(recursive_inputs) / sizeof(recursive_inputs[0]);
       ++i) {
    long input = recursive_inputs[i];
    long got = c_recursive(input);
    long expected = c_recursive_reference(input);
    if (got != expected) {
      fprintf(stderr, "C recursive call mismatch for %ld: %ld != %ld\n", input,
              got, expected);
      return 1;
    }
  }
  static const long loop_counts[] = {-1, 0, 1, 2, 9, 64};
  for (size_t i = 0; i < sizeof(loop_counts) / sizeof(loop_counts[0]); ++i) {
    long empty_got = c_empty_statements(loop_counts[i]);
    long empty_expected = c_empty_statements_reference(loop_counts[i]);
    if (empty_got != empty_expected) {
      fprintf(stderr, "C empty-statement mismatch for %ld: %ld != %ld\n",
              loop_counts[i], empty_got, empty_expected);
      return 1;
    }
    long got = c_while_sum(loop_counts[i]);
    long expected = c_while_sum_reference(loop_counts[i]);
    if (got != expected) {
      fprintf(stderr, "C while-loop mismatch for %ld: %ld != %ld\n",
              loop_counts[i], got, expected);
      return 1;
    }
    got = c_for_sum(loop_counts[i]);
    expected = c_for_sum_reference(loop_counts[i]);
    if (got != expected) {
      fprintf(stderr, "C for-loop mismatch for %ld: %ld != %ld\n",
              loop_counts[i], got, expected);
      return 1;
    }
    got = c_for_multi_declarators(loop_counts[i]);
    expected = c_for_multi_declarators_reference(loop_counts[i]);
    if (got != expected) {
      fprintf(stderr,
              "C for-loop multi-declarator mismatch for %ld: %ld != %ld\n",
              loop_counts[i], got, expected);
      return 1;
    }
    got = c_nested_for_sum(loop_counts[i]);
    expected = c_nested_for_sum_reference(loop_counts[i]);
    if (got != expected) {
      fprintf(stderr, "C nested for-loop mismatch for %ld: %ld != %ld\n",
              loop_counts[i], got, expected);
      return 1;
    }
    got = c_while_continue(loop_counts[i]);
    expected = c_while_continue_reference(loop_counts[i]);
    if (got != expected) {
      fprintf(stderr, "C while-continue mismatch for %ld: %ld != %ld\n",
              loop_counts[i], got, expected);
      return 1;
    }
    got = c_while_break(loop_counts[i]);
    expected = c_while_break_reference(loop_counts[i]);
    if (got != expected) {
      fprintf(stderr, "C while-break mismatch for %ld: %ld != %ld\n",
              loop_counts[i], got, expected);
      return 1;
    }
    got = c_nested_while_control(loop_counts[i]);
    expected = c_nested_while_control_reference(loop_counts[i]);
    if (got != expected) {
      fprintf(stderr, "C nested-while control mismatch for %ld: %ld != %ld\n",
              loop_counts[i], got, expected);
      return 1;
    }
    got = c_for_continue(loop_counts[i]);
    expected = c_for_continue_reference(loop_counts[i]);
    if (got != expected) {
      fprintf(stderr, "C for-continue mismatch for %ld: %ld != %ld\n",
              loop_counts[i], got, expected);
      return 1;
    }
    got = c_for_break(loop_counts[i]);
    expected = c_for_break_reference(loop_counts[i]);
    if (got != expected) {
      fprintf(stderr, "C for-break mismatch for %ld: %ld != %ld\n",
              loop_counts[i], got, expected);
      return 1;
    }
    got = c_nested_for_control(loop_counts[i]);
    expected = c_nested_for_control_reference(loop_counts[i]);
    if (got != expected) {
      fprintf(stderr, "C nested-loop control mismatch for %ld: %ld != %ld\n",
              loop_counts[i], got, expected);
      return 1;
    }
    got = c_for_shadow(loop_counts[i]);
    expected = c_for_shadow_reference(loop_counts[i]);
    if (got != expected) {
      fprintf(stderr, "C for-loop shadow mismatch for %ld: %ld != %ld\n",
              loop_counts[i], got, expected);
      return 1;
    }
    got = c_for_assign_sum(loop_counts[i]);
    expected = c_for_assign_sum_reference(loop_counts[i]);
    if (got != expected) {
      fprintf(stderr, "C for-loop assignment mismatch for %ld: %ld != %ld\n",
              loop_counts[i], got, expected);
      return 1;
    }
    got = c_for_body_step(loop_counts[i]);
    expected = c_for_body_step_reference(loop_counts[i]);
    if (got != expected) {
      fprintf(stderr, "C for-loop body-step mismatch for %ld: %ld != %ld\n",
              loop_counts[i], got, expected);
      return 1;
    }
    got = c_while_early_return(loop_counts[i]);
    expected = c_while_early_return_reference(loop_counts[i]);
    if (got != expected) {
      fprintf(stderr, "C while-loop return mismatch for %ld: %ld != %ld\n",
              loop_counts[i], got, expected);
      return 1;
    }
    got = c_for_early_return(loop_counts[i]);
    expected = c_for_early_return_reference(loop_counts[i]);
    if (got != expected) {
      fprintf(stderr, "C for-loop return mismatch for %ld: %ld != %ld\n",
              loop_counts[i], got, expected);
      return 1;
    }
  }
  static const long inputs[][2] = {
      {0, 0}, {1, 2}, {-7, 4}, {1000, -17}, {123456, 789},
  };
  for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
    long got = c_kernel(inputs[i][0], inputs[i][1]);
    long expected = c_reference(inputs[i][0], inputs[i][1]);
    if (got != expected) {
      fprintf(stderr, "C frontend mismatch for (%ld, %ld): %ld != %ld\n",
              inputs[i][0], inputs[i][1], got, expected);
      return 1;
    }
    got = c_choose(inputs[i][0], inputs[i][1]);
    expected = c_choose_reference(inputs[i][0], inputs[i][1]);
    if (got != expected) {
      fprintf(stderr, "C control-flow mismatch for (%ld, %ld): %ld != %ld\n",
              inputs[i][0], inputs[i][1], got, expected);
      return 1;
    }
    got = c_locals(inputs[i][0], inputs[i][1]);
    expected = c_locals_reference(inputs[i][0], inputs[i][1]);
    if (got != expected) {
      fprintf(stderr, "C local-variable mismatch for (%ld, %ld): %ld != %ld\n",
              inputs[i][0], inputs[i][1], got, expected);
      return 1;
    }
    got = c_local_update(inputs[i][0], inputs[i][1]);
    expected = c_local_update_reference(inputs[i][0], inputs[i][1]);
    if (got != expected) {
      fprintf(stderr, "C local-update mismatch for (%ld, %ld): %ld != %ld\n",
              inputs[i][0], inputs[i][1], got, expected);
      return 1;
    }
    got = c_multi_declarator(inputs[i][0]);
    expected = c_multi_declarator_reference(inputs[i][0]);
    if (got != expected) {
      fprintf(stderr, "C multi-declarator mismatch for %ld: %ld != %ld\n",
              inputs[i][0], got, expected);
      return 1;
    }
    got = c_declare_then_assign(inputs[i][0], inputs[i][1]);
    expected = c_declare_then_assign_reference(inputs[i][0], inputs[i][1]);
    if (got != expected) {
      fprintf(stderr,
              "C uninitialized-local assignment mismatch for (%ld, "
              "%ld): %ld != %ld\n",
              inputs[i][0], inputs[i][1], got, expected);
      return 1;
    }
    got = c_parameter_update(inputs[i][0]);
    expected = c_parameter_update_reference(inputs[i][0]);
    if (got != expected) {
      fprintf(stderr, "C parameter-update mismatch for %ld: %ld != %ld\n",
              inputs[i][0], got, expected);
      return 1;
    }
    got = c_call(inputs[i][0], inputs[i][1]);
    expected = c_call_reference(inputs[i][0], inputs[i][1]);
    if (got != expected) {
      fprintf(stderr, "C function-call mismatch for (%ld, %ld): %ld != %ld\n",
              inputs[i][0], inputs[i][1], got, expected);
      return 1;
    }
  }
  static const long values[] = {0, 1, -1, 42, -900};
  for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
    long got = c_predicate(values[i]);
    long expected = c_predicate_reference(values[i]);
    if (got != expected) {
      fprintf(stderr, "C logical-not mismatch for %ld: %ld != %ld\n", values[i],
              got, expected);
      return 1;
    }
  }
  static const long stored_values[] = {-11, 0, 42, 900};
  for (size_t i = 0; i < sizeof(stored_values) / sizeof(stored_values[0]);
       ++i) {
    long got_slot = 17;
    long expected_slot = 17;
    long got = c_store(&got_slot, stored_values[i]);
    long expected = c_store_reference(&expected_slot, stored_values[i]);
    if (got != expected || got_slot != expected_slot) {
      fprintf(stderr,
              "C pointer-store mismatch for %ld: (%ld, %ld) != (%ld, %ld)\n",
              stored_values[i], got, got_slot, expected, expected_slot);
      return 1;
    }
  }
  static const long array_values[] = {17, -8, 0, 91, 2048};
  for (size_t i = 0; i < sizeof(array_values) / sizeof(array_values[0]); ++i) {
    long got_items[] = {17, -8, 0, 91, 2048};
    long expected_items[] = {17, -8, 0, 91, 2048};
    long got = c_index_load(got_items, (long)i);
    long expected = c_index_load_reference(expected_items, (long)i);
    if (got != expected) {
      fprintf(stderr, "C indexed-load mismatch at %zu: %ld != %ld\n", i, got,
              expected);
      return 1;
    }
    if (c_pointer_add(got_items, (long)i) !=
            c_pointer_add_reference(expected_items, (long)i) ||
        c_pointer_add_reversed(got_items, (long)i) !=
            c_pointer_add_reversed_reference(expected_items, (long)i) ||
        c_pointer_subtract(got_items, (long)i) !=
            c_pointer_subtract_reference(expected_items, (long)i) ||
        c_pointer_compound_offset(got_items, (long)i) !=
            c_pointer_compound_offset_reference(expected_items, (long)i) ||
        (i == 0 &&
         (c_pointer_postfix_increment(got_items) !=
              c_pointer_postfix_increment_reference(expected_items) ||
          c_pointer_prefix_increment(got_items) !=
              c_pointer_prefix_increment_reference(expected_items) ||
          c_pointer_prefix_decrement(&got_items[1]) !=
              c_pointer_prefix_decrement_reference(&expected_items[1]) ||
          c_const_data_pointer_increment(got_items) !=
              c_const_data_pointer_increment_reference(expected_items)))) {
      fprintf(stderr, "C pointer arithmetic mismatch at %zu\n", i);
      return 1;
    }
    got = c_i64_array(got_items, (long)i);
    expected = c_i64_array_reference(expected_items, (long)i);
    if (got != expected) {
      fprintf(stderr, "C array-parameter mismatch at %zu: %ld != %ld\n", i, got,
              expected);
      return 1;
    }
    got = c_i64_array_forward(got_items, (long)i);
    expected = c_i64_array_forward_reference(expected_items, (long)i);
    if (got != expected) {
      fprintf(stderr, "C integer pointer-call mismatch at %zu: %ld != %ld\n", i,
              got, expected);
      return 1;
    }
    got = c_index_store(got_items, (long)i, -((long)i + 29));
    expected =
        c_index_store_reference(expected_items, (long)i, -((long)i + 29));
    if (got != expected || got_items[i] != expected_items[i]) {
      fprintf(stderr,
              "C indexed-store mismatch at %zu: (%ld, %ld) != (%ld, %ld)\n", i,
              got, got_items[i], expected, expected_items[i]);
      return 1;
    }
    got = c_i64_array_store(got_items, (long)i, -((long)i * 7 + 3));
    expected = c_i64_array_store_reference(expected_items, (long)i,
                                           -((long)i * 7 + 3));
    if (got != expected || got_items[i] != expected_items[i]) {
      fprintf(stderr, "C array-parameter store mismatch at %zu\n", i);
      return 1;
    }
  }
  static const long signed_shift_values[] = {-1024, -3, -1, 0, 1, 1024};
  for (size_t i = 0;
       i < sizeof(signed_shift_values) / sizeof(signed_shift_values[0]); ++i) {
    for (size_t j = 0; j < sizeof(shift_amounts) / sizeof(shift_amounts[0]);
         ++j) {
      long value = signed_shift_values[i];
      long amount = shift_amounts[j];
      if (c_shift_right(value, amount) !=
          c_shift_right_reference(value, amount)) {
        fprintf(stderr, "C signed right-shift mismatch for (%ld, %ld)\n", value,
                amount);
        return 1;
      }
    }
  }
  static const long flags[] = {0, 1, -1};
  for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); ++i) {
    long got = c_if_increment(12, flags[i]);
    long expected = c_if_increment_reference(12, flags[i]);
    if (got != expected) {
      fprintf(stderr, "C single-branch if mismatch for %ld: %ld != %ld\n",
              flags[i], got, expected);
      return 1;
    }
    got = c_if_early_return(flags[i]);
    expected = c_if_early_return_reference(flags[i]);
    if (got != expected) {
      fprintf(stderr, "C early-return if mismatch for %ld: %ld != %ld\n",
              flags[i], got, expected);
      return 1;
    }
  }
  static const long compound_values[] = {-100, -1, 0, 27};
  for (size_t i = 0; i < sizeof(compound_values) / sizeof(compound_values[0]);
       ++i) {
    long got = c_compound(compound_values[i]);
    long expected = c_compound_reference(compound_values[i]);
    if (got != expected) {
      fprintf(stderr, "C compound-scope mismatch for %ld: %ld != %ld\n",
              compound_values[i], got, expected);
      return 1;
    }
  }
  for (long value = -2; value <= 5; ++value) {
    long got = c_shadow_local(value);
    long expected = c_shadow_local_reference(value);
    if (got != expected) {
      fprintf(stderr, "C block-shadow mismatch for %ld: %ld != %ld\n", value,
              got, expected);
      return 1;
    }
  }
  static const double truth_values[] = {0.0, -0.0, 0.5, -2.0};
  for (size_t i = 0; i < sizeof(truth_values) / sizeof(truth_values[0]); ++i) {
    long got = c_double_truth(truth_values[i]);
    long expected = c_double_truth_reference(truth_values[i]);
    if (got != expected) {
      fprintf(stderr, "C double-condition mismatch at %zu: %ld != %ld\n", i,
              got, expected);
      return 1;
    }
  }
  for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
    long got = c_bitwise(inputs[i][0], inputs[i][1]);
    long expected = c_bitwise_reference(inputs[i][0], inputs[i][1]);
    if (got != expected) {
      fprintf(stderr, "C bitwise mismatch for (%ld, %ld): %ld != %ld\n",
              inputs[i][0], inputs[i][1], got, expected);
      return 1;
    }
  }
  for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
    long got = c_load(&values[i]);
    long expected = c_load_reference(&values[i]);
    if (got != expected) {
      fprintf(stderr, "C pointer-load mismatch for %ld: %ld != %ld\n",
              values[i], got, expected);
      return 1;
    }
  }
  static const double pointer_values[] = {0.0, -0.0, 1.5, -7.25, 1024.125};
  for (size_t i = 0; i < sizeof(pointer_values) / sizeof(pointer_values[0]);
       ++i) {
    double got = c_f64_load(&pointer_values[i]);
    double expected = c_f64_load_reference(&pointer_values[i]);
    if (memcmp(&got, &expected, sizeof(got)) != 0) {
      fprintf(stderr, "C double pointer-load mismatch at %zu\n", i);
      return 1;
    }
    if (memcmp(
            &(double){c_double_pointer_add(pointer_values, (long)i)},
            &(double){c_double_pointer_add_reference(pointer_values, (long)i)},
            sizeof(double)) != 0) {
      fprintf(stderr, "C double pointer arithmetic mismatch at %zu\n", i);
      return 1;
    }
    double compound_items[] = {0.0, -0.0, 1.5, -7.25, 1024.125};
    double compound_reference_items[] = {0.0, -0.0, 1.5, -7.25, 1024.125};
    if (memcmp(&(double){c_double_pointer_compound_offset(compound_items,
                                                          (long)i)},
               &(double){c_double_pointer_compound_offset_reference(
                   compound_reference_items, (long)i)},
               sizeof(double)) != 0) {
      fprintf(stderr, "C double pointer compound mismatch at %zu\n", i);
      return 1;
    }
    double increment_items[] = {0.0, -0.0, 1.5, -7.25, 1024.125};
    double increment_reference_items[] = {0.0, -0.0, 1.5, -7.25, 1024.125};
    if (i == 0 &&
        memcmp(&(double){c_double_pointer_postfix_increment(increment_items)},
               &(double){c_double_pointer_postfix_increment_reference(
                   increment_reference_items)},
               sizeof(double)) != 0) {
      fprintf(stderr, "C double pointer increment mismatch at %zu\n", i);
      return 1;
    }
    double got_items[] = {0.0, -0.0, 1.5, -7.25, 1024.125};
    double expected_items[] = {0.0, -0.0, 1.5, -7.25, 1024.125};
    got = c_f64_index(got_items, (long)i);
    expected = c_f64_index_reference(expected_items, (long)i);
    if (memcmp(&got, &expected, sizeof(got)) != 0) {
      fprintf(stderr, "C double indexed-load mismatch at %zu\n", i);
      return 1;
    }
    got = c_f64_array(got_items, (long)i);
    expected = c_f64_array_reference(expected_items, (long)i);
    if (memcmp(&got, &expected, sizeof(got)) != 0) {
      fprintf(stderr, "C double array-parameter mismatch at %zu\n", i);
      return 1;
    }
    got = c_f64_array_forward(got_items, (long)i);
    expected = c_f64_array_forward_reference(expected_items, (long)i);
    if (memcmp(&got, &expected, sizeof(got)) != 0) {
      fprintf(stderr, "C double pointer-call mismatch at %zu\n", i);
      return 1;
    }
    double new_value = (double)i * -3.25 + 0.5;
    got = c_f64_array_store(got_items, (long)i, new_value);
    expected = c_f64_array_store_reference(expected_items, (long)i, new_value);
    if (memcmp(&got, &expected, sizeof(got)) != 0 ||
        memcmp(&got_items[i], &expected_items[i], sizeof(got)) != 0) {
      fprintf(stderr, "C double array-parameter store mismatch at %zu\n", i);
      return 1;
    }
    got = c_f64_index_store(got_items, (long)i, new_value);
    expected = c_f64_index_store_reference(expected_items, (long)i, new_value);
    if (memcmp(&got, &expected, sizeof(got)) != 0 ||
        memcmp(&got_items[i], &expected_items[i], sizeof(got)) != 0) {
      fprintf(stderr, "C double indexed-store mismatch at %zu\n", i);
      return 1;
    }
    double got_slot = pointer_values[i];
    double expected_slot = pointer_values[i];
    got = c_f64_store(&got_slot, new_value);
    expected = c_f64_store_reference(&expected_slot, new_value);
    if (memcmp(&got, &expected, sizeof(got)) != 0 ||
        memcmp(&got_slot, &expected_slot, sizeof(got)) != 0) {
      fprintf(stderr, "C double pointer-store mismatch at %zu\n", i);
      return 1;
    }
  }
  static const double float_inputs[][2] = {
      {0.0, 0.0},
      {1.5, 2.25},
      {-3.5, 0.125},
      {100.25, -0.5},
  };
  for (size_t i = 0; i < sizeof(float_inputs) / sizeof(float_inputs[0]); ++i) {
    double got = c_double(float_inputs[i][0], float_inputs[i][1]);
    double expected =
        c_double_reference(float_inputs[i][0], float_inputs[i][1]);
    if (got != expected) {
      fprintf(stderr, "C double mismatch for (%.17g, %.17g): %.17g != %.17g\n",
              float_inputs[i][0], float_inputs[i][1], got, expected);
      return 1;
    }
    got = c_double_choose(float_inputs[i][0], float_inputs[i][1]);
    expected =
        c_double_choose_reference(float_inputs[i][0], float_inputs[i][1]);
    if (got != expected) {
      fprintf(stderr,
              "C double comparison mismatch for (%.17g, %.17g): %.17g != "
              "%.17g\n",
              float_inputs[i][0], float_inputs[i][1], got, expected);
      return 1;
    }
    got = c_double_local(float_inputs[i][0], float_inputs[i][1]);
    expected = c_double_local_reference(float_inputs[i][0], float_inputs[i][1]);
    if (got != expected) {
      fprintf(stderr,
              "C double local-variable mismatch for (%.17g, %.17g): %.17g != "
              "%.17g\n",
              float_inputs[i][0], float_inputs[i][1], got, expected);
      return 1;
    }
    got = c_double_local_update(float_inputs[i][0], float_inputs[i][1]);
    expected =
        c_double_local_update_reference(float_inputs[i][0], float_inputs[i][1]);
    if (got != expected) {
      fprintf(stderr,
              "C double local-update mismatch for (%.17g, %.17g): %.17g != "
              "%.17g\n",
              float_inputs[i][0], float_inputs[i][1], got, expected);
      return 1;
    }
    got = c_double_call(float_inputs[i][0], float_inputs[i][1]);
    expected = c_double_call_reference(float_inputs[i][0], float_inputs[i][1]);
    if (got != expected) {
      fprintf(stderr,
              "C double function-call mismatch for (%.17g, %.17g): %.17g != "
              "%.17g\n",
              float_inputs[i][0], float_inputs[i][1], got, expected);
      return 1;
    }
  }
  /*
   * Aggregate ABI: by-value struct parameters, register-class and
   * MEMORY-class aggregate returns, and struct locals.
   */
  struct c_point cp = {11, 31};
  if (c_point_sum(cp) != c_point_sum_reference(cp)) {
    fprintf(stderr, "C struct point-sum mismatch\n");
    return 1;
  }
  struct c_point cq = {-4, 19};
  struct c_point sum = c_point_add(cp, cq);
  struct c_point sum_ref = c_point_add_reference(cp, cq);
  if (sum.x != sum_ref.x || sum.y != sum_ref.y) {
    fprintf(stderr, "C struct point-add mismatch: (%ld,%ld) != (%ld,%ld)\n",
            sum.x, sum.y, sum_ref.x, sum_ref.y);
    return 1;
  }
  struct c_big big = {1, 2, 3, 4};
  if (c_big_sum(big) != c_big_sum_reference(big)) {
    fprintf(stderr, "C struct big-sum (MEMORY class) mismatch\n");
    return 1;
  }
  struct c_big made = c_big_make(7);
  struct c_big made_ref = c_big_make_reference(7);
  if (made.a != made_ref.a || made.b != made_ref.b || made.c != made_ref.c ||
      made.d != made_ref.d) {
    fprintf(stderr, "C struct big-make (sret) mismatch\n");
    return 1;
  }
  struct c_pair pair = {0.5, 0.25};
  if (c_pair_second(pair) != c_pair_second_reference(pair)) {
    fprintf(stderr, "C struct pair-second (SSE class) mismatch\n");
    return 1;
  }
  struct c_pair made_pair = c_pair_make(1.5);
  struct c_pair made_pair_ref = c_pair_make_reference(1.5);
  if (made_pair.first != made_pair_ref.first ||
      made_pair.second != made_pair_ref.second) {
    fprintf(stderr, "C struct pair-make (SSE sret) mismatch\n");
    return 1;
  }
  struct c_mixed mixed = {6, 2.5};
  if (c_mixed_blend(mixed, 5) != c_mixed_blend_reference(mixed, 5)) {
    fprintf(stderr, "C struct mixed param mismatch\n");
    return 1;
  }
  if (c_many_with_struct(1, 2, 3, 4, 5, cp, 9) !=
      c_many_with_struct_reference(1, 2, 3, 4, 5, cp, 9)) {
    fprintf(stderr, "C struct many-args register interleave mismatch\n");
    return 1;
  }
  if (c_struct_array_roundtrip(20) != c_struct_array_roundtrip_reference(20)) {
    fprintf(stderr, "C struct array roundtrip mismatch\n");
    return 1;
  }
  puts("Nyr-backed C frontend: integer/control-flow/memory/double/aggregate "
       "cases matched reference");
  return 0;
}
