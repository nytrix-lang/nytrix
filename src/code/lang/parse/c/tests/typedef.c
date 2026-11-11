/*
 * Parser fixture: define integer and floating-point typedef chains.
 */
typedef long ny_count_t;
typedef ny_count_t ny_index_t;
typedef const ny_count_t ny_readonly_count_t;
typedef double ny_real_t;
typedef int c_mode_word_t __attribute__((__mode__(__word__)));
typedef __SIZE_TYPE__ c_size_type_t;

long c_mode_word_size(void) { return sizeof(c_mode_word_t); }
long c_size_type_size(void) { return sizeof(c_size_type_t); }

long c_mode_word_negative(long value) {
  c_mode_word_t word = value;
  return word < 0;
}

__inline__ long c_gnu_inline_specifier(long value) { return value + 2; }

ny_count_t c_typedef_sum(ny_count_t limit) {
  ny_count_t sum = 0;
  for (ny_index_t index = 0; index < limit; index = index + 1)
    sum += index;
  return sum;
}

ny_count_t c_typedef_add(ny_count_t left, ny_count_t right);

long c_typedef_call(ny_count_t value) { return c_typedef_add(value, 3); }

ny_count_t c_typedef_add(ny_count_t left, ny_count_t right) {
  return left + right;
}

long c_typedef_const(ny_readonly_count_t value) { return value; }

ny_real_t c_typedef_real(ny_real_t left, ny_real_t right) {
  ny_real_t result = left * 2.0 + right;
  return result;
}

long c_typedef_const_update(ny_readonly_count_t value) {
  value = value + 1;
  return value;
}
