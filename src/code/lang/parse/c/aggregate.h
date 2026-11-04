#ifndef NY_CODE_LANG_C_AGGREGATE_H
#define NY_CODE_LANG_C_AGGREGATE_H

/* Aggregate ABI interface shared by the standalone Nyr C frontend and
 * backend.
 *
 * The nyc type model is deliberately minimal: every supported aggregate is a
 * struct or union whose fields are 8-byte scalars (signed long, double,
 * pointers), nested struct/union tags, or arrays of those. Layout follows the
 * usual target alignment rules, and classification mirrors the shared
 * classifiers in code/lang/lower.c so calls and function entry agree. */

#include "code/lang/parse/c/c.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct c_agg_type c_agg_type_t;

typedef enum {
  NYC_ABI_SYSV_X86_64,
  NYC_ABI_I386,
  NYC_ABI_AAPCS64,
  NYC_ABI_WIN64,
  NYC_ABI_UNSUPPORTED
} nyc_abi_t;

typedef enum {
  C_AGG_FIELD_I64,
  C_AGG_FIELD_U64,
  C_AGG_FIELD_I32,
  C_AGG_FIELD_U8,
  C_AGG_FIELD_I16,
  C_AGG_FIELD_U16,
  C_AGG_FIELD_U32,
  C_AGG_FIELD_F32,
  C_AGG_FIELD_F64,
  C_AGG_FIELD_PTR,
  C_AGG_FIELD_AGG,   /* nested struct/union value; agg names the type */
  C_AGG_FIELD_ARRAY, /* elem_count elements; element type is agg for
                        aggregate arrays, otherwise an 8-byte scalar */
} c_agg_field_kind_t;

#define C_AGG_MAX_ARRAY_RANK 32

typedef struct {
  ny_ctok_t name;
  c_agg_field_kind_t kind;
  size_t offset;
  size_t size; /* full byte size including array extents */
  size_t align;
  bool explicit_align; /* alignment attribute raised this field's align */
  unsigned pointer_depth;
  bool pointer_to_aggregate_array;
  bool is_array;
  const c_agg_type_t *agg; /* C_AGG_FIELD_AGG and aggregate arrays */
  size_t elem_count;       /* C_AGG_FIELD_ARRAY (>= 1) */
  unsigned array_rank;
  size_t array_extent[C_AGG_MAX_ARRAY_RANK];
  size_t elem_size;         /* element stride for scalar and aggregate arrays */
  int pointer_element_type; /* NYC C type tag for a pointer field's pointee */
  bool elem_f32;            /* scalar array of float elements */
  bool elem_f64;            /* scalar array of double elements */
  bool elem_unsigned;       /* scalar array of unsigned integers */
  bool elem_pointer;        /* scalar array of pointer elements */
  bool elem_char_pointer;   /* pointer elements whose type is char * */
  bool elem_function_pointer; /* pointer elements whose type is a function
                                 pointer */
  bool is_function_pointer;   /* scalar function-pointer field */
  int function_result_type;   /* nyc C type tag when signature is known */
  const c_agg_type_t *function_result_agg;
  bool has_function_signature;
  bool is_volatile;
  bool is_signed_byte;
  bool is_bool;
  bool pointee_const;
  bool is_bitfield;
  unsigned bit_offset;
  unsigned bit_width;
} c_agg_field_t;

#define C_AGG_MAX_FIELDS 2048
#define C_AGG_MAX_DEPTH 16

struct c_agg_type {
  ny_ctok_t name;
  size_t scope_id;
  bool is_union;
  bool is_int128;
  bool is_transparent_union;
  bool complete;
  size_t size;
  size_t align;
  size_t field_count;
  c_agg_field_t fields[C_AGG_MAX_FIELDS];
  size_t promoted_field_count;
  size_t promoted_field_capacity;
  c_agg_field_t *promoted_fields;
};

/* Classify one complete aggregate per SysV x86-64 rules and pack it like a
 * call-site NYIR_ARG_AGG_* word. Aggregates larger than 16 bytes classify as
 * MEMORY. Returns false for incomplete or unsupported shapes. */
bool nyc_sysv_classify_aggregate(const c_agg_type_t *ty, unsigned depth,
                                 uint32_t *packed_out);

/* Select the supported C aggregate ABI for the target. */
bool nyc_classify_aggregate(const c_agg_type_t *ty, nyc_abi_t abi,
                            unsigned depth, uint32_t *packed_out);
bool nyc_classify_argument(const c_agg_type_t *ty, nyc_abi_t abi,
                           unsigned depth, uint32_t *packed_out);

/* Convenience: true when the packed word denotes the hidden-pointer (sret)
 * return convention. */
static inline bool nyc_sysv_return_is_memory(uint32_t packed) {
  return packed != 0 &&
         ((packed >> 24u) & 0xfu) == 3u /* NYIR_ARG_CLASS_MEMORY */;
}

#endif
