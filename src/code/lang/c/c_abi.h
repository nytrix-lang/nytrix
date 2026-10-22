#ifndef NY_CODE_C_ABI_H
#define NY_CODE_C_ABI_H

/* Aggregate ABI helpers for the standalone Nyr C frontend.
 *
 * The nyc type model is deliberately minimal: every supported aggregate is a
 * struct or union whose fields are 8-byte scalars (signed long, double,
 * pointers), nested struct/union tags, or arrays of those. Layout follows the
 * usual target alignment rules, and classification mirrors the shared
 * classifiers in code/lang/lower.c so calls and function entry agree. */

#include "code/lang/c/c.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct c_agg_type c_agg_type_t;

typedef enum {
  NYC_ABI_SYSV_X86_64,
  NYC_ABI_AAPCS64,
  NYC_ABI_WIN64,
  NYC_ABI_UNSUPPORTED
} nyc_abi_t;

typedef enum {
  C_AGG_FIELD_I64,
  C_AGG_FIELD_F64,
  C_AGG_FIELD_PTR,
  C_AGG_FIELD_AGG,   /* nested struct/union value; agg names the type */
  C_AGG_FIELD_ARRAY, /* elem_count elements; element type is agg for
                        aggregate arrays, otherwise an 8-byte scalar */
} c_agg_field_kind_t;

typedef struct {
  ny_ctok_t name;
  c_agg_field_kind_t kind;
  size_t offset;
  size_t size; /* full byte size including array extents */
  size_t align;
  const c_agg_type_t *agg; /* C_AGG_FIELD_AGG and aggregate arrays */
  size_t elem_count;       /* C_AGG_FIELD_ARRAY (>= 1) */
  bool elem_f64;           /* scalar array of double elements */
} c_agg_field_t;

#define C_AGG_MAX_FIELDS NY_C_MAX_FIELDS
#define C_AGG_MAX_DEPTH 8

struct c_agg_type {
  ny_ctok_t name;
  bool is_union;
  bool complete;
  size_t size;
  size_t align;
  size_t field_count;
  c_agg_field_t fields[C_AGG_MAX_FIELDS];
};

/* Classify one complete aggregate per SysV x86-64 rules and pack it like a
 * call-site NYIR_ARG_AGG_* word. Aggregates larger than 16 bytes classify as
 * MEMORY. Returns false for incomplete or unsupported shapes. */
bool nyc_sysv_classify_aggregate(const c_agg_type_t *ty, unsigned depth,
                                 uint32_t *packed_out);

/* Select the supported C aggregate ABI for the target. */
bool nyc_classify_aggregate(const c_agg_type_t *ty, nyc_abi_t abi,
                            unsigned depth, uint32_t *packed_out);

/* Convenience: true when the packed word denotes the hidden-pointer (sret)
 * return convention. */
static inline bool nyc_sysv_return_is_memory(uint32_t packed) {
  return packed != 0 &&
         ((packed >> 24u) & 0xfu) == 3u /* NYIR_ARG_CLASS_MEMORY */;
}

#endif
