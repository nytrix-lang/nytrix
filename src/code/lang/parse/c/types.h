/*
 * Internal C type model and ABI layout, included by parse.c.
 */
#ifndef NY_CODE_LANG_PARSE_C_TYPES_H
#define NY_CODE_LANG_PARSE_C_TYPES_H

#define C_MAX_ARRAY_RANK 32

typedef enum {
  C_I64,
  C_I32,
  C_U32,
  C_U8,
  C_I16,
  C_U16,
  C_U64,
  C_CHAR,
  C_F32,
  C_F64,
  C_FUNC_PTR,
  C_OPAQUE_PTR,
  C_CHAR_PTR_PTR,
  C_I8_PTR_PTR,
  C_CHAR_PTR,
  C_I8_PTR,
  C_I32_PTR,
  C_U32_PTR,
  C_I16_PTR,
  C_U16_PTR,
  C_I64_PTR,
  C_U64_PTR,
  C_F32_PTR,
  C_F64_PTR,
  C_FUNC_PTR_PTR,
  C_OPAQUE_PTR_PTR,
  C_OPAQUE_PTR_PTR_PTR,
  C_VOID_PTR,
  C_BAD,
  /*
   * SysV x86-64 va_list storage. A local occupies the 24-byte va_list
   * structure; a parameter is adjusted to C_OPAQUE_PTR by parse_signature.
   */
  C_VA_LIST,
  /*
   * By-value struct/union. Values are i64 addresses of storage.
   */
  C_AGG,
  /*
   * Pointer to struct/union.
   */
  C_AGG_PTR,
  /*
   * Pointer to a pointer to struct/union. The pointee type is retained so
   * dereferencing produces a usable aggregate pointer.
   */
  C_AGG_PTR_PTR,
  /*
   * Pointer to a typedef array of aggregates. Dereferencing yields the
   * decayed pointer to its first aggregate element.
   */
  C_AGG_ARRAY_PTR,
  /*
   * Preserve libc's common typed `unsigned short **` return types.
   */
  C_U16_PTR_PTR,
  C_I64_PTR_PTR,
  C_U64_PTR_PTR,
  /*
   * Pointer to an array whose elements are pointers to aggregates.
   */
  C_AGG_PTR_ARRAY_PTR,
  /*
   * Pointer to a SysV x86-64 va_list array.
   */
  C_VA_LIST_PTR
} c_type_t;

typedef struct {
  unsigned rank;
  size_t extent[C_MAX_ARRAY_RANK];
  size_t element_size;
  c_type_t element_type;
  const c_agg_type_t *agg;
  /*
   * The value points at one complete array with this shape.
   */
  bool pointer_to_array;
} c_array_shape_t;

static bool c_type_is_pointer(c_type_t type) {
  return type == C_FUNC_PTR || type == C_OPAQUE_PTR || type == C_CHAR_PTR_PTR ||
         type == C_I8_PTR_PTR || type == C_U16_PTR_PTR || type == C_CHAR_PTR ||
         type == C_I8_PTR || type == C_I32_PTR || type == C_U32_PTR ||
         type == C_I16_PTR || type == C_U16_PTR || type == C_I64_PTR ||
         type == C_U64_PTR || type == C_F32_PTR || type == C_F64_PTR ||
         type == C_FUNC_PTR_PTR || type == C_OPAQUE_PTR_PTR ||
         type == C_OPAQUE_PTR_PTR_PTR || type == C_VOID_PTR ||
         type == C_I64_PTR_PTR || type == C_U64_PTR_PTR || type == C_AGG_PTR ||
         type == C_AGG_PTR_PTR || type == C_AGG_ARRAY_PTR ||
         type == C_AGG_PTR_ARRAY_PTR || type == C_VA_LIST_PTR;
}

static bool c_type_is_object_pointer(c_type_t type) {
  return c_type_is_pointer(type) && type != C_FUNC_PTR;
}

static bool c_pointer_compatible(c_type_t left, c_type_t right) {
  /*
   * `void *` converts implicitly with any object pointer (ISO C) and any
   * function pointer (GNU C, relied on by kernel headers).
   */
  if (left == C_VOID_PTR && c_type_is_pointer(right))
    return true;
  if (right == C_VOID_PTR && c_type_is_pointer(left))
    return true;
  return c_type_is_object_pointer(left) && c_type_is_object_pointer(right);
}

static bool c_type_is_integer(c_type_t type) {
  return type == C_I32 || type == C_U32 || type == C_U8 || type == C_CHAR ||
         type == C_I16 || type == C_U16 || type == C_I64 || type == C_U64;
}

static bool c_type_is_float(c_type_t type) {
  return type == C_F32 || type == C_F64;
}

static size_t c_type_size(nyc_abi_t abi, c_type_t type) {
  if (c_type_is_pointer(type))
    return abi == NYC_ABI_I386 ? 4 : 8;
  return type == C_U8 || type == C_CHAR                    ? 1
         : type == C_I16 || type == C_U16                  ? 2
         : type == C_I32 || type == C_U32 || type == C_F32 ? 4
                                                           : 8;
}

static bool c_cmpxchg_type_compatible(nyc_abi_t abi, c_type_t left,
                                      c_type_t right) {
  bool left_word =
      c_type_is_integer(left) || c_type_is_pointer(left) || left == C_VA_LIST;
  bool right_word = c_type_is_integer(right) || c_type_is_pointer(right) ||
                    right == C_VA_LIST;
  return left_word && right_word &&
         c_type_size(abi, left) == c_type_size(abi, right);
}

static size_t c_type_alignment(nyc_abi_t abi, c_type_t type) {
  size_t size = c_type_size(abi, type);
  if (abi == NYC_ABI_I386 && size > 4)
    return 4;
  return size;
}

static c_type_t c_size_type(nyc_abi_t abi) {
  return abi == NYC_ABI_I386 ? C_U32 : C_U64;
}

static c_type_t c_remove_pointer_levels(c_type_t type, unsigned levels) {
  while (levels--) {
    switch (type) {
    case C_CHAR_PTR:
      type = C_U8;
      break;
    case C_I8_PTR:
      type = C_CHAR;
      break;
    case C_I16_PTR:
      type = C_I16;
      break;
    case C_U16_PTR:
      type = C_U16;
      break;
    case C_U16_PTR_PTR:
      type = C_U16_PTR;
      break;
    case C_I64_PTR_PTR:
      type = C_I64_PTR;
      break;
    case C_U64_PTR_PTR:
      type = C_U64_PTR;
      break;
    case C_I32_PTR:
      type = C_I32;
      break;
    case C_U32_PTR:
      type = C_U32;
      break;
    case C_I64_PTR:
      type = C_I64;
      break;
    case C_U64_PTR:
      type = C_U64;
      break;
    case C_F32_PTR:
      type = C_F32;
      break;
    case C_F64_PTR:
      type = C_F64;
      break;
    case C_AGG_PTR:
      type = C_AGG;
      break;
    case C_AGG_ARRAY_PTR:
      type = C_AGG_PTR;
      break;
    case C_AGG_PTR_ARRAY_PTR:
      type = C_AGG_PTR_PTR;
      break;
    case C_VA_LIST_PTR:
      type = C_OPAQUE_PTR;
      break;
    case C_CHAR_PTR_PTR:
      type = C_CHAR_PTR;
      break;
    case C_I8_PTR_PTR:
      type = C_I8_PTR;
      break;
    case C_AGG_PTR_PTR:
      type = C_AGG_PTR;
      break;
    case C_FUNC_PTR_PTR:
      type = C_FUNC_PTR;
      break;
    case C_OPAQUE_PTR_PTR:
      type = C_OPAQUE_PTR;
      break;
    case C_OPAQUE_PTR_PTR_PTR:
      type = C_OPAQUE_PTR_PTR;
      break;
    case C_OPAQUE_PTR:
      type = C_VOID_PTR;
      break;
    case C_VOID_PTR:
      type = C_BAD;
      break;
    default:
      return C_BAD;
    }
  }
  return type;
}

static c_type_t c_add_pointer_levels(c_type_t base, unsigned levels) {
  if (!levels)
    return base;
  if (levels == 1) {
    switch (base) {
    case C_U8:
      return C_CHAR_PTR;
    case C_CHAR:
      return C_I8_PTR;
    case C_I16:
      return C_I16_PTR;
    case C_U16:
      return C_U16_PTR;
    case C_I32:
      return C_I32_PTR;
    case C_U32:
      return C_U32_PTR;
    case C_I64:
      return C_I64_PTR;
    case C_U64:
      return C_U64_PTR;
    case C_F32:
      return C_F32_PTR;
    case C_F64:
      return C_F64_PTR;
    case C_AGG:
      return C_AGG_PTR;
    case C_AGG_PTR:
      return C_AGG_PTR_PTR;
    case C_VA_LIST:
      return C_VA_LIST_PTR;
    case C_AGG_PTR_PTR:
      return C_OPAQUE_PTR_PTR_PTR;
    case C_AGG_ARRAY_PTR:
      return C_OPAQUE_PTR_PTR;
    case C_CHAR_PTR:
      return C_CHAR_PTR_PTR;
    case C_I8_PTR:
      return C_I8_PTR_PTR;
    case C_U16_PTR:
      return C_U16_PTR_PTR;
    case C_I64_PTR:
      return C_I64_PTR_PTR;
    case C_U64_PTR:
      return C_U64_PTR_PTR;
    case C_OPAQUE_PTR_PTR:
      return C_OPAQUE_PTR_PTR_PTR;
    case C_VOID_PTR:
      return C_OPAQUE_PTR_PTR;
    case C_BAD:
      return C_VOID_PTR;
    case C_FUNC_PTR:
      return C_FUNC_PTR_PTR;
    default:
      return c_type_is_pointer(base) ? C_OPAQUE_PTR : C_BAD;
    }
  }
  if (levels == 2 && base == C_U8)
    return C_CHAR_PTR_PTR;
  if (levels == 2 && base == C_U16)
    return C_U16_PTR_PTR;
  if (levels == 2 && base == C_I64)
    return C_I64_PTR_PTR;
  if (levels == 2 && base == C_U64)
    return C_U64_PTR_PTR;
  if (levels == 2 && base == C_AGG)
    return C_AGG_PTR_PTR;
  if (levels == 2 && base == C_FUNC_PTR)
    return C_FUNC_PTR_PTR;
  if (levels == 2 && base == C_BAD)
    return C_OPAQUE_PTR;
  if (levels >= 3)
    return c_add_pointer_levels(base, 1) == C_BAD ? C_BAD
                                                  : C_OPAQUE_PTR_PTR_PTR;
  return c_add_pointer_levels(base, 1) == C_BAD ? C_BAD : C_OPAQUE_PTR_PTR;
}

static nyr_scalar_class_t c_float_scalar_class(c_type_t type) {
  return type == C_F32 ? NYIR_SCALAR_F32 : NYIR_SCALAR_F64;
}

static bool c_type_is_arithmetic_pointer(c_type_t type) {
  return type == C_CHAR_PTR || type == C_I8_PTR || type == C_I16_PTR ||
         type == C_U16_PTR || type == C_I32_PTR || type == C_U32_PTR ||
         type == C_I64_PTR || type == C_U64_PTR || type == C_F32_PTR ||
         type == C_F64_PTR || type == C_OPAQUE_PTR || type == C_AGG_PTR ||
         type == C_CHAR_PTR_PTR || type == C_I8_PTR_PTR ||
         type == C_U16_PTR_PTR || type == C_OPAQUE_PTR_PTR ||
         type == C_OPAQUE_PTR_PTR_PTR || type == C_AGG_PTR_PTR ||
         type == C_VOID_PTR || type == C_AGG_ARRAY_PTR;
}

static int64_t c_pointer_scale(nyc_abi_t abi, c_type_t type) {
  if (type == C_CHAR_PTR || type == C_I8_PTR || type == C_VOID_PTR)
    return 1;
  if (type == C_I16_PTR || type == C_U16_PTR)
    return 2;
  if (type == C_I32_PTR || type == C_U32_PTR || type == C_F32_PTR)
    return 4;
  return (int64_t)c_type_size(abi, C_VOID_PTR);
}

static int64_t c_pointer_scale_for(nyc_abi_t abi, c_type_t type,
                                   const c_agg_type_t *pointee) {
  return (type == C_AGG_PTR || type == C_AGG_ARRAY_PTR) && pointee &&
                 pointee->complete
             ? (int64_t)pointee->size
             : c_pointer_scale(abi, type);
}

#endif
