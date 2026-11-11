/*
 * Fixture: parse static assertions alongside compatibility declarations.
 */
typedef void nyc_void_alias;
nyc_void_alias nyc_void_alias_probe(void) {}

void nyc_compat_a(unsigned long);
void nyc_compat_b(unsigned long);
void nyc_compat_c(unsigned int);

_Static_assert(__builtin_types_compatible_p(typeof(nyc_compat_a),
                                            typeof(nyc_compat_b)),
               "matching function types");
_Static_assert(!__builtin_types_compatible_p(typeof(nyc_compat_a),
                                             typeof(nyc_compat_c)),
               "different function parameter types");
_Static_assert(1 && 1 && !0, "logical and constant expression");
_Static_assert(0 || 1, "logical or constant expression");
_Static_assert(sizeof(long) * 8 == 64, "host long width");

typedef __signed__ char nyc_signed_char;

enum nyc_packed_enum {
  nyc_packed_one = 1,
  nyc_packed_two = 2
} __attribute__((__packed__));
_Static_assert(sizeof(enum nyc_packed_enum) == 1, "packed enum width");

enum nyc_packed_signed_enum {
  nyc_packed_negative = -1,
  nyc_packed_zero = 0
} __attribute__((packed));
_Static_assert(sizeof(enum nyc_packed_signed_enum) == 1,
               "packed signed enum width");

struct nyc_aligned_field {
  char lead;
  int value __attribute__((__aligned__(8)));
};
_Static_assert(sizeof(struct nyc_aligned_field) == 16, "field alignment");
struct nyc_aligned_type {
  char value;
} __attribute__((aligned(16)));
_Static_assert(sizeof(struct nyc_aligned_type) == 16, "type alignment");
struct nyc_aligned_field_specifier {
  char lead;
  int __attribute__((__aligned__(8))) value;
};
_Static_assert(sizeof(struct nyc_aligned_field_specifier) == 16,
               "specifier alignment");

int nyc_enum_type_probe(void) {
  return sizeof(enum {
      nyc_probe_00, nyc_probe_01, nyc_probe_02, nyc_probe_03, nyc_probe_04,
      nyc_probe_05, nyc_probe_06, nyc_probe_07, nyc_probe_08, nyc_probe_09,
      nyc_probe_10, nyc_probe_11, nyc_probe_12, nyc_probe_13, nyc_probe_14,
      nyc_probe_15, nyc_probe_16, nyc_probe_17, nyc_probe_18, nyc_probe_19,
      nyc_probe_20, nyc_probe_21, nyc_probe_22, nyc_probe_23, nyc_probe_24,
      nyc_probe_25, nyc_probe_26, nyc_probe_27, nyc_probe_28, nyc_probe_29,
      nyc_probe_30, nyc_probe_31, nyc_probe_32});
}
int nyc_static_assert(void) { return (nyc_signed_char)-1 < 0 ? 0 : 1; }

_Static_assert((((96 + (16 - 1)) & ~(16 - 1)) / sizeof(long)) == 12,
               "bitwise constant expression in array bound");
_Static_assert(__builtin_bswap16(0x0800) == 0x0008,
               "byte-swap constant expression");
_Static_assert(__builtin_strlen("kernel"
                                "x") == 7,
               "builtin string length constant expression");

typedef char nyc_zero_length_array[0];
struct nyc_zero_length_field {
  int before;
  nyc_zero_length_array empty;
  int after;
};
_Static_assert(sizeof(struct nyc_zero_length_field) == 8,
               "zero-length typedef array size");
_Static_assert(__builtin_offsetof(struct nyc_zero_length_field, after) == 4,
               "zero-length typedef array offset");

_Static_assert(sizeof(_Float32) == 4, "_Float32 size");
_Static_assert(sizeof(_Float64) == 8, "_Float64 size");
_Static_assert(sizeof(_Float32x) == 8, "_Float32x size");
_Float32 nyc_float32_identity(_Float32 x) { return x; }
_Float64 nyc_float64_identity(_Float64 x) { return x; }
_Float32x nyc_float32x_identity(_Float32x x) { return x; }

struct nyc_has_attribute_probe {
  _Static_assert(!__builtin_has_attribute(0, nonstring), "unannotated");
};
_Static_assert(sizeof(struct {
                 _Static_assert(!__builtin_has_attribute(0, nonstring),
                                "unannotated");
               }) == 0,
               "empty GNU aggregate");

void nyc_kernel_ignored_attrs(void)
    __attribute__((__noclone__, __no_profile_instrument_function__));
struct nyc_kernel_nonstring_field {
  char signature[4] __attribute__((__nonstring__));
};

typedef __builtin_va_list va_list;
struct nyc_va_list_field {
  va_list args;
};
_Static_assert(sizeof(struct nyc_va_list_field) == 24,
               "SysV va_list field size");

struct nyc_match_token {
  int token;
  const char *pattern;
};
typedef struct nyc_match_token nyc_match_table[];
int nyc_incomplete_array_param(nyc_match_table table) { return table[0].token; }

int nyc_block_extern_function(void) {
  extern int nyc_late_external(int);
  return nyc_late_external(7);
}
int nyc_block_extern_array(void) {
  extern unsigned char nyc_late_external_array[];
  return nyc_late_external_array[0];
}
struct nyc_incomplete_value;
struct nyc_incomplete_owner {
  struct nyc_incomplete_value **items;
};
int nyc_incomplete_pointer_field_index(struct nyc_incomplete_owner *owner,
                                       int index) {
  return owner->items[index] != 0;
}
int nyc_opaque_pointer_array_index(void **items, int index) {
  return items[index] != 0;
}
void *nyc_atomic_pointer_load(void **slot) { return __atomic_load_n(slot, 0); }
void nyc_atomic_pointer_store(void **slot, void *value) {
  __atomic_store_n(slot, value, 0);
}
void nyc_store_u16_pointer(unsigned short **slot, unsigned short *value) {
  *slot = value;
}
extern int nyc_accept_void_pointer(const void *pointer);
int nyc_pass_u16_pointer_pointer_to_void(unsigned short **pointer) {
  return nyc_accept_void_pointer(pointer);
}
void nyc_write_debug_register(unsigned long value) {
  __asm__("mov %0, %%db0" : : "r"(value));
}
