struct nyc_atomic { int counter; };
struct nyc_lock { struct nyc_atomic tail; };
typedef unsigned long nyc_regset_t[4];
static char nyc_storage[4] = "abc";
static const unsigned int __attribute__((aligned(64)))
    nyc_aligned_values[2] = {1, 2};
static char *nyc_current = nyc_storage;
static unsigned long nyc_fallback;
typedef long long nyc_long_long;
static nyc_long_long nyc_global_long_long;
static int nyc_set_atomic(struct nyc_atomic *value) {
  value->counter = 17; return value->counter;
}
static int nyc_read_regset(nyc_regset_t *regs) {
  (*regs)[0] = 7; return (*regs)[0] != 7;
}
static int nyc_statement_expression_local_label(int exit_early) {
  return ({ __label__ done; int value = 0; if (exit_early) goto done;
            value = 41; done: value + 1; });
}
static int nyc_static_matrix(void) {
  enum { nyc_audio_type, nyc_spdif_type };
  static const int indices[2][5] = {
      [nyc_audio_type] = {0, 2, 4, 5, -1},
      [nyc_spdif_type] = {1, -1},
  };
  return indices[0][3] != 5 || indices[1][0] != 1 || indices[1][2] != 0;
}
static int nyc_narrow_overflow(void) {
  unsigned char byte = 0;
  unsigned short word = 0;
  short signed_word = 0;
  return !__builtin_add_overflow(255, 1, &byte) || byte != 0 ||
         !__builtin_add_overflow(65535, 1, &word) || word != 0 ||
         !__builtin_add_overflow(32767, 1, &signed_word) ||
         signed_word != -32768;
}
static int nyc_static_local_assignment(void) {
  static int value;
  value = 7;
  value += 3;
  return value != 10;
}
static int nyc_pragma_loop(void) {
  int count = 0;
  _Pragma("GCC unroll 2")
  for (int i = 0; i < 2; ++i)
    ++count;
  return count != 2;
}
static int nyc_builtin_ctz(void) {
  return __builtin_ctz(1U) != 0 || __builtin_ctz(0x80000000U) != 31 ||
         __builtin_ctz(0x00001000U) != 12;
}
static __attribute__((__target__("lzcnt,bmi,bmi2"))) int
nyc_bmi2_target_hint(void) {
  return __builtin_ctz(8U) != 3;
}
#if defined(__GNUC__) && !defined(__clang__)
static __attribute__((optimize("no-tree-vectorize"))) int
nyc_no_vectorize_hint(unsigned int value) {
#else
static int nyc_no_vectorize_hint(unsigned int value) {
#endif
  return value + 1 != 9;
}
static int nyc_cpuid_partial_outputs(void) {
  unsigned int max_leaf = 0;
  unsigned int leaf1_ecx = 0;
  unsigned int leaf1_edx = 0;
  unsigned int leaf7_ebx = 0;
  unsigned int leaf7_ecx = 0;
  __asm__ volatile("cpuid"
                   : "=a"(max_leaf)
                   : "a"(0)
                   : "ebx", "ecx", "edx");
  if (max_leaf == 0)
    return 1;
  __asm__ volatile("cpuid"
                   : "=a"(max_leaf), "=c"(leaf1_ecx), "=d"(leaf1_edx)
                   : "a"(1)
                   : "ebx");
  __asm__ volatile("cpuid"
                   : "=a"(max_leaf), "=b"(leaf7_ebx), "=c"(leaf7_ecx)
                   : "a"(7), "c"(0)
                   : "edx");
  return max_leaf == 0 && leaf1_ecx == 0 && leaf1_edx == 0 &&
         leaf7_ebx == 0 && leaf7_ecx == 0;
}
static int nyc_dynamic_choose_expr(unsigned int value) {
  return __builtin_choose_expr(__builtin_constant_p(value), 99, value + 3) !=
         value + 3;
}
static int nyc_generic_unsigned_char_pointer(void) {
  return _Generic((const unsigned char *)0, const char *: 1,
                  const unsigned char *: 2, default: 3) != 2;
}
static int nyc_generic_long_long(void) {
  nyc_long_long value = 0;
  return _Generic(value, long: 1, long long: 2, default: 3) != 2 ||
         _Generic((long long)0, long: 1, long long: 2, default: 3) != 2;
}
static int nyc_generic_long_long_parameter(nyc_long_long value) {
  return _Generic(value, long: 1, long long: 2, default: 3) != 2;
}
static int nyc_generic_global_long_long(void) {
  return _Generic(nyc_global_long_long, long: 1, long long: 2, default: 3) !=
         2;
}
static int nyc_aggregate_array_copy(struct nyc_atomic *source) {
  struct nyc_atomic values[2] = {*source};
  return values[0].counter != 31 || values[1].counter != 0;
}
int main(void) {
  struct nyc_lock lock;
  nyc_regset_t nyc_regs;
  int parenthesized_increment = 0;
  if (++(parenthesized_increment) != 1)
    return 6;
  if (((unsigned long)nyc_aligned_values & 63) != 0 ||
      nyc_aligned_values[1] != 2)
    return 5;
  if (nyc_static_matrix())
    return 7;
  if (nyc_narrow_overflow())
    return 12;
  if (nyc_static_local_assignment())
    return 13;
  if (nyc_pragma_loop())
    return 24;
  if (nyc_builtin_ctz())
    return 25;
  if (nyc_bmi2_target_hint())
    return 27;
  if (nyc_no_vectorize_hint(8))
    return 28;
  if (nyc_cpuid_partial_outputs())
    return 26;
  if (nyc_dynamic_choose_expr(5))
    return 14;
  if (nyc_generic_unsigned_char_pointer())
    return 19;
  if (nyc_generic_long_long())
    return 20;
  if (nyc_generic_long_long_parameter(0))
    return 21;
  if (nyc_generic_global_long_long())
    return 22;
  if (nyc_global_long_long != 0)
    return 23;
  lock.tail.counter = 31;
  if (nyc_aggregate_array_copy(&lock.tail))
    return 15;
  unsigned long *selected = _Generic((void *)0,
      void *: &(unsigned long){ 42 }, default: &nyc_fallback);
  lock.tail.counter = 0;
  nyc_current += 1;
  nyc_current[1] = 'Z';
  if (nyc_set_atomic(&lock.tail) != 17) return 1;
  if (nyc_read_regset(&nyc_regs) || *selected != 42) return 2;
  if (nyc_statement_expression_local_label(1) != 1 ||
      nyc_statement_expression_local_label(0) != 42) return 4;
  return nyc_storage[2] != 'Z' ? 3 : 0;
}
