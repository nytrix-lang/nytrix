/*
 * C preprocessor fixture for __COUNTER__ across trailing static assertions.
 */
typedef unsigned long u64;
typedef unsigned int u32;
_Static_assert(__COUNTER__ == 0 && __COUNTER__ == 1,
               "preprocessor counter increments per expansion");
struct __attribute__((__packed__)) hv_enlightenments_control {
  u32 nested_flush_hypercall : 1;
  u32 msr_bitmap : 1;
  u32 enlightened_npt_tlb : 1;
  u32 reserved : 29;
} __attribute__((__packed__)) hv_enlightenments_control;
_Static_assert(sizeof(hv_enlightenments_control) == 4,
               "pre-tag packed aggregate attribute");
struct hv_vpset {
  u64 format;
  u64 valid_bank_mask;
  u64 bank_contents[];
};
struct hv_tlb_flush_ex {
  u64 address_space;
  u64 flags;
  union {
    struct hv_vpset hv_vp_set;
    struct {
      unsigned char
          __offset_to_FAM[__builtin_offsetof(struct hv_vpset, bank_contents)];
      u64 gva_list[];
    } __attribute__((__packed__));
  };
} __attribute__((__packed__));
_Static_assert(__builtin_offsetof(struct hv_tlb_flush_ex,
                                  hv_vp_set.bank_contents) ==
                   __builtin_offsetof(struct hv_tlb_flush_ex, gva_list),
               "overlaid flexible-array member offsets");
_Static_assert(__builtin_offsetof(struct hv_tlb_flush_ex,
                                  hv_vp_set.bank_contents) == 32,
               "nested flexible-array member has the expected byte offset");
struct nyc_array_size_initializer {
  int size;
};
static struct nyc_array_size_initializer nyc_array_size_initializer = {
    .size = sizeof(char[8])};
struct nyc_global_size_source {
  long max_files;
};
static struct nyc_global_size_source nyc_global_size_source;
static struct nyc_array_size_initializer nyc_global_member_size = {
    .size = sizeof(nyc_global_size_source.max_files)};
struct nyc_signed_array_extent {
  int tasks[(1 + 2 - (-1))];
};
_Static_assert(sizeof(struct nyc_signed_array_extent) == 4 * sizeof(int),
               "array extent arithmetic handles parenthesized negatives");
struct nyc_three_dim_array {
  unsigned long values[2][3][4];
};
_Static_assert(sizeof(struct nyc_three_dim_array) == 24 * sizeof(unsigned long),
               "three-dimensional aggregate arrays retain full size");
typedef typeof(1 + 1) nyc_arithmetic_typeof;
_Static_assert(sizeof(nyc_arithmetic_typeof) == sizeof(int),
               "typeof arithmetic expression retains its scalar type");
static int nyc_section_integer_array[3] __attribute__((section(".nyc_test")));
void nyc_write_section_integer_array(unsigned index) {
  nyc_section_integer_array[index] = 5;
}
struct nyc_pointer_array_element {
  int value;
};
struct nyc_pointer_array_owner {
  struct nyc_pointer_array_element *owners[3];
};
struct nyc_pointer_array_element **
nyc_pointer_array_offset(struct nyc_pointer_array_owner *owner,
                         unsigned index) {
  return owner->owners + index;
}
static unsigned long nyc_designated_integer_matrix[2][2] = {
    [1] = {[0] = 11, [1] = 13},
};
enum nyc_designated_index { nyc_designated_first, nyc_designated_second };
struct nyc_designated_array_element {
  int value;
};
static struct nyc_designated_array_element nyc_designated_array[] = {
    [nyc_designated_first] = {.value = 3},
    [nyc_designated_second] = {.value = 7},
};
int trailing_overlap_assert_probe(void) { return 0; }
