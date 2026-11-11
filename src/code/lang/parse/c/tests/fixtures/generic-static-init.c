/*
 * Fixture: initialize generic-dispatch metadata in static storage.
 */
struct nyc_generic_unrelated {
  int value;
};
struct nyc_generic_namespace {
  int value;
};
struct nyc_generic_common {
  unsigned type;
  const int *ops;
};
typedef int (*nyc_callback_t)(void);
extern const nyc_callback_t nyc_callback_alias_table[];
typedef struct {
  int value;
} nyc_attribute_state_t;
nyc_attribute_state_t __attribute__((__noinline__))
__attribute__((no_instrument_function))
__attribute((__section__(".nyc.noinstr")))
__attribute__((__no_sanitize_address__))
nyc_attribute_probe(nyc_attribute_state_t *state);
struct nyc_static_target {
  int value;
};
struct nyc_static_holder {
  struct nyc_static_target *pointer;
};
static struct nyc_static_target nyc_static_object;
static struct nyc_static_holder nyc_static_holder_value = {
    .pointer = (&nyc_static_object)};
struct nyc_scalar_array_holder {
  unsigned long values[4];
};
static struct nyc_scalar_array_holder nyc_scalar_array_value = {
    .values = {[0 ... 2] = 7, [3] = 8}};
struct nyc_pair {
  unsigned current, maximum;
};
struct nyc_pair_holder {
  struct nyc_pair values[3];
};
static struct nyc_generic_namespace nyc_generic_namespace;
static const int nyc_generic_ops;
static __attribute__((section(".init.data"))) unsigned long nyc_scalar_a,
    nyc_scalar_b, nyc_scalar_c, nyc_scalar_d;
static int nyc_callback_zero(void) { return 0; }
static int nyc_callback_two(void) { return 2; }
const nyc_callback_t nyc_callback_alias_table[] = {nyc_callback_zero,
                                                   nyc_callback_two};
static unsigned long nyc_braced_scalar = {9};
static struct nyc_pair_holder nyc_pair_values = {
    .values = {[0] = {1, 2}, [2] = {3, 4}}};
static int (*nyc_callback_table[3])(void) = {
    [0] = nyc_callback_zero,
    [2] = nyc_callback_two,
};
const char nyc_generic_banner[];
const char nyc_generic_banner[] = "boot banner";
static struct nyc_generic_common nyc_generic_common = {
    .type = _Generic((&nyc_generic_namespace),
        struct nyc_generic_unrelated *: 1,
        struct nyc_generic_namespace *: 0x04000000),
    .ops = _Generic((&nyc_generic_namespace),
        struct nyc_generic_unrelated *: &nyc_generic_ops,
        struct nyc_generic_namespace *: (1 ? &nyc_generic_ops : ((void *)0)))};
struct nyc_note_extent {
  unsigned a, b, c;
};
#define nyc_align_extent(x, a)                                   \
  (((x) + ((__typeof__(x))(a) - 1)) & ~((__typeof__(x))(a) - 1))
typedef unsigned nyc_u32;
typedef nyc_u32
    nyc_note_buf_t[((nyc_align_extent(sizeof(struct nyc_note_extent), 4) * 2) +
                    nyc_align_extent(sizeof("CORE"), 4) +
                    nyc_align_extent(sizeof(struct nyc_note_extent), 4)) /
                   4];
extern typeof(nyc_generic_common) nyc_generic_common;
static int nyc_dynamic_alloca(unsigned long size) {
  unsigned char *bytes = (unsigned char *)__builtin_alloca(size);
  bytes[0] = 37;
  return bytes[0];
}
int main(void) {
  if (nyc_dynamic_alloca(64) != 37)
    return 1;
  if (nyc_callback_alias_table[0] == 0 || nyc_callback_alias_table[1] == 0)
    return 1;
  if (nyc_generic_common.type != 0x04000000)
    return 1;
  if (nyc_generic_common.ops != &nyc_generic_ops)
    return 2;
  if (nyc_generic_banner[10] != 'r')
    return 3;
  if (nyc_braced_scalar != 9)
    return 4;
  if (nyc_static_holder_value.pointer != &nyc_static_object)
    return 5;
  if (nyc_scalar_array_value.values[0] != 7 ||
      nyc_scalar_array_value.values[2] != 7 ||
      nyc_scalar_array_value.values[3] != 8)
    return 6;
  if (nyc_pair_values.values[0].maximum != 2 ||
      nyc_pair_values.values[1].current != 0 ||
      nyc_pair_values.values[2].maximum != 4)
    return 7;
  nyc_scalar_a = 1;
  nyc_scalar_b = 2;
  nyc_scalar_c = 3;
  nyc_scalar_d = 4;
  if (nyc_callback_table[0]() != 0 || nyc_callback_table[1] != 0 ||
      nyc_callback_table[2]() != 2)
    return 6;
  return nyc_scalar_a + nyc_scalar_b + nyc_scalar_c + nyc_scalar_d != 10 ? 7
                                                                         : 0;
}
