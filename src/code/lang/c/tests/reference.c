long c_reference(long left, long right) { return left * 3 + right; }

long c_choose_reference(long left, long right) {
  if (left < right)
    return left + right;
  return left - right;
}

long c_predicate_reference(long value) { return !value; }

long c_bitwise_reference(long left, long right) {
  return (~left & right) | (left ^ right);
}

long c_shift_left_reference(long value, long amount) { return value << amount; }

long c_shift_right_reference(long value, long amount) {
  return value >> amount;
}

long c_shift_precedence_reference(long value) { return (value + 2) << 1; }

long c_shift_additive_precedence_reference(long value) {
  return value << (1 + 1);
}

long c_shift_compound_reference(long value) {
  value <<= 2;
  value >>= 1;
  return value;
}

static long c_logic_mutate_reference(long *slot) {
  *slot = 1;
  return 7;
}

long c_logic_and_skip_reference(long *slot) {
  return 0 && c_logic_mutate_reference(slot);
}

long c_logic_or_skip_reference(long *slot) {
  return 1 || c_logic_mutate_reference(slot);
}

long c_logic_and_run_reference(long *slot) {
  return 1 && c_logic_mutate_reference(slot);
}

long c_logic_or_run_reference(long *slot) {
  return 0 || c_logic_mutate_reference(slot);
}

long c_logic_and_precedence_reference(void) { return 1 + 1 && 0; }

long c_logic_or_precedence_reference(void) { return 2 * 0 || 1; }

static long c_cond_mutate_reference(long *slot) {
  *slot = 1;
  return 7;
}

long c_cond_true_reference(long *slot) {
  return 2 ? c_cond_mutate_reference(slot) : 9;
}

long c_cond_false_reference(long *slot) {
  return 0 ? c_cond_mutate_reference(slot) : 9;
}

long c_cond_precedence_reference(long value) { return value + 1 ? 7 : 9; }

long c_cond_nested_reference(long value) { return value ? 0 : (1 ? 7 : 9); }

long c_prefix_increment_reference(long value) { return ++value; }

long c_prefix_decrement_reference(long value) { return --value; }

long c_postfix_update_reference(long value) {
  long previous = value++;
  return previous * 10 + value;
}

long c_postfix_decrement_reference(long value) {
  long previous = value--;
  return previous * 10 + value;
}

long c_for_increment_reference(long count) {
  long sum = 0;
  for (long index = 0; index < count; ++index)
    sum = sum + index;
  return sum;
}

long c_while_increment_reference(long count) {
  long index = 0;
  long sum = 0;
  while (index < count) {
    sum = sum + index;
    index++;
  }
  return sum;
}

long c_do_sum_reference(long count) {
  long index = 0;
  long sum = 0;
  do {
    if (index == 2) {
      index++;
      continue;
    }
    sum = sum + index;
    index++;
  } while (index < count);
  return sum;
}

long c_do_break_reference(long count) {
  long index = 0;
  long sum = 0;
  do {
    index++;
    if (index == 3)
      break;
    sum = sum + index;
  } while (index < count);
  return sum;
}

long c_do_nested_conditional_reference(long count) {
  long iterations = 0;
  if (count > 0)
    do {
      iterations++;
    } while (iterations < count);
  return iterations;
}

double c_double_postfix_reference(double value) {
  double previous = value++;
  return previous + value;
}

long c_load_reference(const long *pointer) { return *pointer; }

double c_f64_load_reference(const double *pointer) { return *pointer; }

long c_store_reference(long *slot, long value) {
  *slot = value;
  return *slot;
}

double c_f64_store_reference(double *slot, double value) {
  *slot = value;
  return *slot;
}

long c_locals_reference(long left, long right) {
  long sum = left + right;
  long doubled = sum * 2;
  return doubled - left;
}

long c_many_locals_reference(void) {
  long sum = 0;
  for (long value = 0; value <= 32; ++value)
    sum += value;
  return sum;
}

long c_many_functions_reference(void) { return 42; }

long c_target_link_reference(void) { return 42; }

long c_seven_params_reference(long a, long b, long c, long d, long e, long f,
                              long g) {
  return a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6 + g * 7;
}

long c_seven_call_reference(long a, long b, long c, long d, long e, long f,
                            long g) {
  return c_seven_params_reference(a, b, c, d, e, f, g);
}

long c_ten_params_reference(long a, long b, long c, long d, long e, long f,
                            long g, long h, long i, long j) {
  return a + b * 3 + c * 5 + d * 7 + e * 11 + f * 13 + g * 17 + h * 19 +
         i * 23 + j * 29;
}

long c_ten_call_reference(long a, long b, long c, long d, long e, long f,
                          long g, long h, long i, long j) {
  return c_ten_params_reference(a, b, c, d, e, f, g, h, i, j);
}

double c_ten_doubles_reference(double a, double b, double c, double d, double e,
                               double f, double g, double h, double i,
                               double j) {
  return a + b * 2.0 + c * 3.0 + d * 4.0 + e * 5.0 + f * 6.0 + g * 7.0 +
         h * 8.0 + i * 9.0 + j * 10.0;
}

double c_ten_double_call_reference(double a, double b, double c, double d,
                                   double e, double f, double g, double h,
                                   double i, double j) {
  return c_ten_doubles_reference(a, b, c, d, e, f, g, h, i, j);
}

long c_recursive_reference(long value) {
  if (value < 2)
    return value;
  return value * c_recursive_reference(value - 1);
}

long c_empty_statements_reference(long count) {
  long sum = 0;
  ;
  while (count > 0) {
    sum += count;
    --count;
    ;
  }
  for (; count < 2; ++count)
    ;
  if (sum)
    ;
  else {
    ;
  }
  return sum;
}

long c_local_update_reference(long left, long right) {
  long value = left;
  value = value + right;
  return value;
}

long c_multi_declarator_reference(long seed) {
  long first = seed, second = seed + 2, result;
  result = first + second;
  return result;
}

long c_declare_then_assign_reference(long left, long right) {
  long value;
  value = left + right;
  return value;
}

long c_parameter_update_reference(long value) {
  value = value + 1;
  return value;
}

long c_call_reference(long left, long right) {
  long helper = left * 3 + right;
  return helper - 7;
}

long c_while_sum_reference(long count) {
  long index = 0;
  long sum = 0;
  while (index < count) {
    sum = sum + index;
    index = index + 1;
  }
  return sum;
}

long c_while_early_return_reference(long count) {
  while (count > 0)
    return count + 7;
  return -1;
}

long c_while_continue_reference(long count) {
  long index = 0;
  long sum = 0;
  while (index < count) {
    index = index + 1;
    if (index == 2)
      continue;
    sum = sum + index;
  }
  return sum;
}

long c_while_break_reference(long count) {
  long index = 0;
  long sum = 0;
  while (index < count) {
    index = index + 1;
    if (index == 3)
      break;
    sum = sum + index;
  }
  return sum;
}

long c_nested_while_control_reference(long count) {
  long outer = 0;
  long sum = 0;
  while (outer < count) {
    long inner = 0;
    while (inner < count) {
      inner = inner + 1;
      if (inner == 2)
        continue;
      if (inner == 4)
        break;
      sum = sum + outer + inner;
    }
    outer = outer + 1;
  }
  return sum;
}

long c_for_sum_reference(long count) {
  long sum = 0;
  for (long index = 0; index < count; index = index + 1)
    sum = sum + index;
  return sum;
}

long c_for_multi_declarators_reference(long count) {
  long sum = 0;
  for (long index = 0, weight = 1; index < count; ++index) {
    sum += index * weight;
    ++weight;
  }
  return sum;
}

long c_for_assign_sum_reference(long count) {
  long index = 0;
  long sum = 0;
  for (index = 0; index < count; index = index + 1)
    sum = sum + index;
  return sum;
}

long c_for_body_step_reference(long count) {
  long index = 0;
  long sum = 0;
  for (; index < count;) {
    sum = sum + index;
    index = index + 1;
  }
  return sum;
}

long c_for_early_return_reference(long count) {
  for (long index = 0; index < count; index = index + 1)
    return index + count;
  return -1;
}

long c_for_shadow_reference(long seed) {
  long index = seed;
  for (long index = 0; index < 3; index = index + 1)
    seed = seed + index;
  return seed + index;
}

long c_nested_for_sum_reference(long count) {
  long sum = 0;
  for (long outer = 0; outer < count; outer = outer + 1) {
    for (long inner = 0; inner < count; inner = inner + 1)
      sum = sum + outer + inner;
  }
  return sum;
}

long c_for_continue_reference(long count) {
  long sum = 0;
  for (long index = 0; index < count; index = index + 1) {
    if (index == 2)
      continue;
    sum = sum + index;
  }
  return sum;
}

long c_for_break_reference(long count) {
  long sum = 0;
  for (long index = 0; index < count; index = index + 1) {
    if (index == 3)
      break;
    sum = sum + index;
  }
  return sum;
}

long c_nested_for_control_reference(long count) {
  long sum = 0;
  for (long outer = 0; outer < count; outer = outer + 1) {
    for (long inner = 0; inner < count; inner = inner + 1) {
      if (inner == 1)
        continue;
      if (inner == 3)
        break;
      sum = sum + outer + inner;
    }
  }
  return sum;
}

long c_index_load_reference(const long *items, long index) {
  return items[index];
}

long c_i64_array_reference(const long items[], long index) {
  return items[index];
}

long c_i64_array_forward_reference(const long items[], long index) {
  return c_i64_array_reference(items, index);
}

long c_pointer_add_reference(const long *items, long index) {
  return *(items + index);
}

long c_pointer_add_reversed_reference(const long *items, long index) {
  return *(index + items);
}

long c_pointer_subtract_reference(const long *items, long index) {
  return *(items + 4 - index);
}

long c_pointer_compound_offset_reference(long *items, long index) {
  long *current = items;
  current += index;
  long selected = *current;
  current -= index;
  return selected + *current;
}

long c_pointer_postfix_increment_reference(long *items) {
  long *current = items;
  long previous = *current++;
  return previous + *current;
}

long c_pointer_prefix_increment_reference(long *items) {
  long *current = items;
  return *++current;
}

long c_pointer_prefix_decrement_reference(long *items) {
  long *current = items + 1;
  return *--current;
}

long c_const_data_pointer_increment_reference(const long *items) {
  const long *current = items;
  long previous = *current++;
  current = items;
  current += 1;
  return previous + *current;
}

long c_i64_array_store_reference(long items[], long index, long value) {
  items[index] = value;
  return items[index];
}

long c_index_store_reference(long *items, long index, long value) {
  items[index] = value;
  return items[index];
}

double c_f64_index_reference(const double *items, long index) {
  return items[index];
}

double c_f64_array_reference(const double items[], long index) {
  return items[index];
}

double c_double_pointer_add_reference(const double *items, long index) {
  return *(items + index);
}

double c_double_pointer_compound_offset_reference(double *items, long index) {
  double *current = items;
  current += index;
  double selected = *current;
  current -= index;
  return selected + *current;
}

double c_double_pointer_postfix_increment_reference(double *items) {
  double *current = items;
  double previous = *current++;
  return previous + *current;
}

double c_f64_array_forward_reference(const double items[], long index) {
  return c_f64_array_reference(items, index);
}

double c_f64_array_store_reference(double items[], long index, double value) {
  items[index] = value;
  return items[index];
}

double c_f64_index_store_reference(double *items, long index, double value) {
  items[index] = value;
  return items[index];
}

long c_if_increment_reference(long value, long flag) {
  if (flag)
    value = value + 1;
  return value;
}

long c_double_truth_reference(double value) {
  if (value)
    return 1;
  else
    return 0;
}

long c_if_early_return_reference(long value) {
  if (value)
    return value;
  return 0;
}

long c_compound_reference(long value) {
  long output = value;
  {
    long temporary = output + 3;
    output = temporary * 2;
  }
  return output;
}

long c_compound_assignments_reference(long value) {
  long result = value;
  result += 7;
  result -= 2;
  result *= 3;
  result /= 3;
  result %= 11;
  result &= 15;
  result |= 2;
  result ^= 1;
  return result;
}

double c_double_compound_assignment_reference(double value) {
  double result = value;
  result += 1.5;
  result *= 2.0;
  result -= 0.5;
  result /= 2.0;
  return result;
}

long c_pointer_compound_assignment_reference(long *slot, long value) {
  *slot += value;
  return *slot;
}

long c_index_compound_assignment_reference(long *items, long index,
                                           long value) {
  items[index] ^= value;
  return items[index];
}

double c_double_pointer_compound_assignment_reference(double *slot,
                                                      double value) {
  *slot *= value;
  return *slot;
}

long c_shadow_local_reference(long value) {
  long shadow = value;
  {
    long shadow = 3;
    value = value + shadow;
  }
  return shadow + value;
}

double c_double_reference(double left, double right) {
  return left * 2.0 + right;
}

double c_double_choose_reference(double left, double right) {
  if (left < right)
    return left;
  else
    return right;
}

double c_double_local_reference(double left, double right) {
  double total = left + right;
  return total * 2.0;
}

double c_double_local_update_reference(double left, double right) {
  double value = left;
  value = value + right;
  return value * 2.0;
}

double c_double_call_reference(double left, double right) {
  double helper = left * 2.0 + right;
  return helper + 1.0;
}

void c_void_store_reference(long *out, long value) { *out = value; }

void c_void_store_return_reference(long *out, long value) {
  *out = value;
  return;
}

long c_void_call_reference(long *out, long value) {
  c_void_store_return_reference(out, value);
  return *out;
}

void *c_void_pointer_sink(void *pointer) { return pointer; }

void *c_void_pointer_identity_reference(void *pointer) {
  return c_void_pointer_sink(pointer);
}

double c_mixed_abi_reference(long i0, double d0, long i1, double d1, long i2,
                             double d2, long i3, double d3, long i4, double d4,
                             long i5, double d5, long i6, double d6, double d7,
                             double d8) {
  if (i0 != 1 || i1 != 2 || i2 != 3 || i3 != 4 || i4 != 5 || i5 != 6 || i6 != 7)
    return -1.0;
  return d0 + d1 + d2 + d3 + d4 + d5 + d6 + d7 + d8;
}

double c_mixed_abi_call_reference(long i0, double d0, long i1, double d1,
                                  long i2, double d2, long i3, double d3,
                                  long i4, double d4, long i5, double d5,
                                  long i6, double d6, double d7, double d8) {
  return c_mixed_abi_reference(i0, d0, i1, d1, i2, d2, i3, d3, i4, d4, i5, d5,
                               i6, d6, d7, d8);
}

/*
 * Aggregate ABI reference twins (structs.c).
 */
struct c_point { long x; long y; };
struct c_big { long a; long b; long c; long d; };
struct c_pair { double first; double second; };
struct c_mixed { long tag; double value; };

long c_point_sum_reference(struct c_point p) { return p.x + p.y; }

struct c_point c_point_add_reference(struct c_point a, struct c_point b) {
  struct c_point r;
  r.x = a.x + b.x;
  r.y = a.y + b.y;
  return r;
}

long c_big_sum_reference(struct c_big b) { return b.a + b.b + b.c + b.d; }

struct c_big c_big_make_reference(long seed) {
  struct c_big b;
  b.a = seed;
  b.b = seed + 1;
  b.c = seed + 2;
  b.d = seed + 3;
  return b;
}

double c_pair_second_reference(struct c_pair p) {
  struct c_pair copy = p;
  return copy.second;
}

struct c_pair c_pair_make_reference(double base) {
  struct c_pair p;
  p.first = base;
  p.second = base * 2.0;
  return p;
}

long c_mixed_blend_reference(struct c_mixed m, long scale) {
  return m.tag * scale;
}

long c_many_with_struct_reference(long a, long b, long c, long d, long e,
                                  struct c_point f, long g) {
  return a + b + c + d + e + f.x + f.y + g;
}

long c_struct_array_roundtrip_reference(long seed) {
  struct c_point points[3];
  long i = 0;
  while (i < 3) {
    points[i].x = seed + i;
    points[i].y = seed - i;
    i += 1;
  }
  struct c_point total;
  total.x = 0;
  total.y = 0;
  i = 0;
  while (i < 3) {
    total = points[i];
    i += 1;
  }
  return total.x * 10 + total.y;
}
