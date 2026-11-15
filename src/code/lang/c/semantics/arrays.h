/*
 * Internal array-shape and decay operations, included by parse.c.
 */
#include "code/lang/c/semantics/types.h"

static size_t c_array_shape_size(const c_array_shape_t *shape) {
  if (!shape || !shape->rank || !shape->element_size)
    return 0;
  size_t size = shape->element_size;
  for (unsigned dimension = 0; dimension < shape->rank; ++dimension) {
    size_t extent = shape->extent[dimension];
    if (!extent || size > SIZE_MAX / extent)
      return 0;
    size *= extent;
  }
  return size;
}

static c_type_t c_array_pointer_type(c_type_t type) {
  return type == C_U8              ? C_CHAR_PTR
         : type == C_CHAR          ? C_I8_PTR
         : type == C_I16           ? C_I16_PTR
         : type == C_U16           ? C_U16_PTR
         : type == C_I32           ? C_I32_PTR
         : type == C_U32           ? C_U32_PTR
         : type == C_I64           ? C_I64_PTR
         : type == C_U64           ? C_U64_PTR
         : type == C_F32           ? C_F32_PTR
         : type == C_F64           ? C_F64_PTR
         : type == C_AGG           ? C_AGG_PTR
         : type == C_CHAR_PTR      ? C_CHAR_PTR_PTR
         : type == C_I8_PTR        ? C_I8_PTR_PTR
         : type == C_U16_PTR       ? C_U16_PTR_PTR
         : type == C_I64_PTR       ? C_I64_PTR_PTR
         : type == C_U64_PTR       ? C_U64_PTR_PTR
         : type == C_FUNC_PTR      ? C_FUNC_PTR_PTR
         : type == C_AGG_PTR       ? C_AGG_PTR_PTR
         : c_type_is_pointer(type) ? C_OPAQUE_PTR_PTR
                                   : C_I64_PTR;
}

static c_type_t c_array_decay_type(c_type_t type) {
  return type == C_AGG ? C_AGG_PTR : c_array_pointer_type(type);
}
