/*
 * C parser fixture for builtin offsetof on an aligned structure member.
 */
struct nyc_offset_probe {
  char first;
  long second;
};
_Static_assert(sizeof(((struct nyc_offset_probe *)0)->second) == sizeof(long),
               "sizeof aggregate member constant expression");
_Static_assert(L'\0' == 0, "wide null character constant");
struct nyc_sizeof_array_member {
  unsigned char bytes[48];
};
#define NYC_SIZEOF_FIELD(TYPE, MEMBER) sizeof((((TYPE *)0)->MEMBER))
_Static_assert(
    NYC_SIZEOF_FIELD(struct nyc_sizeof_array_member, bytes) == 48,
    "sizeof_field preserves an array member under extra parentheses");
struct nyc_typeof_array_element {
  int value;
};
const struct nyc_typeof_array_element *nyc_typeof_pointer_array[2];
extern typeof(nyc_typeof_pointer_array) nyc_typeof_pointer_array;
extern unsigned char nyc_extern_byte_array[18];
unsigned char nyc_extern_byte_array[18] = {[0] = 1};
extern typeof(nyc_extern_byte_array) nyc_extern_byte_array;
extern const unsigned char nyc_incomplete_byte_array[];
const unsigned char nyc_incomplete_byte_array[] = {1, 2, 3};
extern typeof(nyc_incomplete_byte_array) nyc_incomplete_byte_array;
struct nyc_flexible_matrix {
  unsigned short count;
  const int values[][4];
};
_Static_assert(sizeof(struct nyc_flexible_matrix) == 4,
               "flexible array member may have a fixed inner extent");
_Static_assert(__builtin_offsetof(struct nyc_flexible_matrix, values[0]) == 4,
               "flexible-array element zero has the member offset");
static const struct nyc_flexible_matrix nyc_flexible_matrix_data = {
    .count = 2,
    .values = {{11, 12, 13, 14}, {21, 22, 23, 24}},
};
struct nyc_flexible_string_table {
  unsigned int count;
  const char *strings[];
};
static const struct nyc_flexible_string_table nyc_flexible_string_table_data = {
    .count = 2,
    .strings = {"alpha", "beta"},
};
static const char *nyc_casted_string_pointer = (void *)"casted";
struct nyc_self_sized_initializer {
  unsigned int count;
};
static struct nyc_self_sized_initializer nyc_self_sized_initializer[2] = {
    {.count = sizeof(nyc_self_sized_initializer) /
              sizeof(struct nyc_self_sized_initializer)},
};
typedef unsigned long nyc_aligned4_t __attribute__((aligned(4)));
struct nyc_aligned_typedef_probe {
  char first;
  nyc_aligned4_t second;
};
_Static_assert(__alignof__(nyc_aligned4_t) == 4, "typedef alignment attribute");
_Static_assert(__builtin_offsetof(struct nyc_aligned_typedef_probe, second) ==
                   4,
               "aligned typedef member offset");
_Static_assert(__builtin_offsetof(struct nyc_offset_probe, second) == 8,
               "builtin offsetof constant expression");
_Static_assert(__alignof__(unsigned long) == 8, "alignof constant expression");
_Static_assert(__alignof__(struct {
                 unsigned char c;
                 unsigned long value;
               }) == 8,
               "anonymous aggregate alignment constant expression");
_Static_assert(sizeof(struct { long value; } __attribute__((aligned))) == 16,
               "anonymous aggregate largest-alignment size");
_Static_assert(sizeof(1UL - 1) == 8,
               "sizeof expression uses its evaluated type");
struct nyc_tentative_aggregate {
  long value;
};
static struct nyc_tentative_aggregate nyc_tentative_object;
static struct nyc_tentative_aggregate nyc_tentative_object;
static struct nyc_tentative_aggregate nyc_tentative_object = {.value = 7};
extern int nyc_extern_tentative;
int nyc_extern_tentative;
static long nyc_address_source = 17;
static unsigned long nyc_integer_address = (unsigned long)&nyc_address_source;
_Static_assert((~0U) == 4294967295ULL, "unsigned-int complement width");
_Static_assert(~(unsigned int)0 == 4294967295ULL,
               "cast unsigned-int complement width");
struct nyc_clz_probe {
  char bytes[__builtin_clzll(1ULL)];
};
enum nyc_high_bit_probe { NYC_HIGH_BIT = 1U << 31 };
_Static_assert(NYC_HIGH_BIT == 2147483648ULL, "unsigned enum constant width");
enum nyc_wide_enum_probe { NYC_WIDE_MASK = 0xffffffffULL << 32 };
_Static_assert(sizeof(enum nyc_wide_enum_probe) == 8,
               "wide unsigned enum representation");
_Static_assert(__bits_per(3) == 2, "kernel bits_per constant expression");
_Static_assert(__builtin_constant_p(3) == 1,
               "constant_p integer constant expression");
_Static_assert(__builtin_choose_expr(1, 3, 4) == 3,
               "choose_expr constant expression");
_Static_assert(__builtin_choose_expr(0, 3, 4) == 4,
               "choose_expr false constant expression");
_Static_assert(__builtin_bswap32(0x12345678U) == 0x78563412U,
               "32-bit byte swap constant expression");
_Static_assert(__builtin_bswap64(0x0123456789abcdefULL) ==
                   0xefcdab8967452301ULL,
               "64-bit byte swap constant expression");
#define NYC_CONSTEXPR(x)                                                \
  (sizeof(int) == sizeof(*(8 ? ((void *)((long)(x) * 0l)) : (int *)8)))
#define NYC_CONST_TRUE(x) __builtin_choose_expr(NYC_CONSTEXPR(x), x, 0)
#define NYC_BUILD_BUG_ON_ZERO(e)                                    \
  ((int)sizeof(struct { _Static_assert(!(e), "constant check"); }))
_Static_assert(NYC_CONSTEXPR((0) > 11), "kernel integer constant probe");
_Static_assert(NYC_BUILD_BUG_ON_ZERO(NYC_CONST_TRUE((0) > 11)) == 0,
               "kernel BUILD_BUG_ON_ZERO constant expression");
#define NYC_IS_SIGNED_TYPE(type) (((typeof(type))-1) < (typeof(type))1)
#define NYC_TYPE_HALF_MAX(type)                                  \
  ((type)1 << (8 * sizeof(type) - 1 - NYC_IS_SIGNED_TYPE(type)))
#define NYC_TYPE_MAX(type)                                          \
  ((type)((NYC_TYPE_HALF_MAX(type) - 1) + NYC_TYPE_HALF_MAX(type)))
#define NYC_GENMASK_ULL(h, l)                                              \
  ((unsigned long long)(NYC_BUILD_BUG_ON_ZERO(NYC_CONST_TRUE((l) > (h))) + \
                        (NYC_TYPE_MAX(unsigned long long) << (l) &         \
                         NYC_TYPE_MAX(unsigned long long) >>               \
                             (sizeof(unsigned long long) * 8 - 1 - (h)))))
_Static_assert(NYC_TYPE_MAX(unsigned long long) == 0xFFFFFFFFFFFFFFFFULL,
               "unsigned 64-bit constant wrap");
_Static_assert((0xFEULL & NYC_GENMASK_ULL(11, 0)) == 0xFEULL &&
                   (0x1FFUL & NYC_GENMASK_ULL(11, 0)) == 0x1FFUL &&
                   (0xFFFUL & NYC_GENMASK_ULL(11, 0)) == 0xFFFUL,
               "GENMASK_ULL constant expressions");
_Static_assert((0 ?: 8) == 8 && (3 ?: 8) == 3,
               "GNU omitted-middle conditional expression");
struct nyc_ilog2_probe {
  char bytes[__ilog2_u64(128ULL)];
};
struct nyc_constant_probe {
  char bytes[__builtin_constant_p(8ULL)];
};
_Static_assert(sizeof(struct nyc_clz_probe) == 63, "clzll constant expression");
_Static_assert(sizeof(struct nyc_ilog2_probe) == 7,
               "ilog2 constant expression");
_Static_assert(sizeof(struct nyc_constant_probe) == 1,
               "constant-p array bound");
struct nyc_page_probe {
  char bytes[64];
};
#define nyc_ilog2(n)                                         \
  (__builtin_constant_p(n)                                   \
       ? ((n) < 2 ? 0 : 63 - __builtin_clzll(n))             \
       : (sizeof(n) <= 4 ? __ilog2_u32(n) : __ilog2_u64(n)))
#define nyc_vmemmap_tail_count                                       \
  (nyc_ilog2(0x400000000ULL) - 12 -                                  \
   nyc_ilog2(2 * ((1UL) << 12) / sizeof(struct nyc_page_probe)) + 1)
struct nyc_vmemmap_probe {
  void *tails[nyc_vmemmap_tail_count > 0 ? nyc_vmemmap_tail_count : 0];
};
_Static_assert(sizeof(struct nyc_vmemmap_probe) == 16 * sizeof(void *),
               "Linux vmemmap tail array bound");
enum {
  nyc_fixmap_probe =
      (((((-10UL << 20) + (1UL << 12) - 1) |
         (__typeof__((-10UL << 20) + (1UL << 12))((1 << 21) - 1))) +
        1) -
       (1UL << 12) - (-10UL << 20)) >>
  12
};
_Static_assert(nyc_fixmap_probe == 511, "Linux fixed-map enum expression");
unsigned long nyc_parenthesized_param(unsigned long(value)) { return value; }
void *nyc_assume_aligned_probe(void) __attribute__((__assume_aligned__(16)));
int nyc_offset_probe(void) {
  return nyc_tentative_object.value != 7 || nyc_extern_tentative != 0 ||
         nyc_integer_address != (unsigned long)&nyc_address_source;
}
