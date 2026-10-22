/*
 * SysV x86-64 aggregate classification for the standalone Nyr C frontend.
 * The merge logic mirrors ny_native_sysv_classify_aggregate_depth in
 * code/lang/lower.c so extern declarations and nyc-defined functions agree
 * on the ABI without sharing parser state.
 */

#include "code/lang/c/c_abi.h"

#include <nyr/ir.h>

static bool c_abi_aapcs_hfa(const c_agg_type_t *ty, unsigned depth,
                            unsigned *kind_out, unsigned *count_out) {
  if (!ty || !ty->complete || !ty->field_count || depth > C_AGG_MAX_DEPTH)
    return false;
  size_t expected_offset = 0;
  unsigned kind = NYIR_ARG_CLASS_NONE;
  unsigned count = 0;
  for (size_t i = 0; i < ty->field_count; ++i) {
    const c_agg_field_t *field = &ty->fields[i];
    unsigned field_kind = NYIR_ARG_CLASS_NONE;
    unsigned field_count = 0;
    if (field->kind == C_AGG_FIELD_F64) {
      field_kind = NYIR_ARG_CLASS_HFA_F64;
      field_count = 1;
    } else if (field->kind == C_AGG_FIELD_ARRAY && field->agg) {
      unsigned nested_kind = 0, nested_count = 0;
      if (!field->elem_count ||
          !c_abi_aapcs_hfa(field->agg, depth + 1, &nested_kind,
                           &nested_count) ||
          nested_count > 4u / field->elem_count)
        return false;
      field_kind = nested_kind;
      field_count = nested_count * (unsigned)field->elem_count;
    } else if (field->kind == C_AGG_FIELD_ARRAY && field->elem_f64) {
      if (!field->elem_count || field->elem_count > 4)
        return false;
      field_kind = NYIR_ARG_CLASS_HFA_F64;
      field_count = (unsigned)field->elem_count;
    } else if (field->kind == C_AGG_FIELD_AGG) {
      if (!c_abi_aapcs_hfa(field->agg, depth + 1, &field_kind,
                           &field_count))
        return false;
    } else {
      return false;
    }
    if (!field_count || field_count > 4 ||
        (kind != NYIR_ARG_CLASS_NONE && kind != field_kind) ||
        field->offset != (ty->is_union ? 0 : expected_offset) ||
        field->offset > ty->size ||
        field->size > ty->size - field->offset ||
        field->size != (size_t)field_count * sizeof(double))
      return false;
    kind = field_kind;
    if (ty->is_union) {
      if (field_count > count)
        count = field_count;
    } else {
      if (field_count > 4u - count)
        return false;
      count += field_count;
      expected_offset += field->size;
    }
  }
  if (!count || count > 4 ||
      (ty->is_union ? ty->size != count * sizeof(double)
                    : expected_offset != ty->size))
    return false;
  *kind_out = kind;
  *count_out = count;
  return true;
}

static bool c_abi_aapcs_classify(const c_agg_type_t *ty, unsigned depth,
                                 uint32_t *packed_out) {
  if (!ty || !packed_out || !ty->complete || ty->size == 0 ||
      depth > C_AGG_MAX_DEPTH || ty->size > NYIR_ARG_AGG_SIZE_MASK)
    return false;
  unsigned hfa_kind = NYIR_ARG_CLASS_NONE, hfa_count = 0;
  if (c_abi_aapcs_hfa(ty, depth, &hfa_kind, &hfa_count)) {
    *packed_out = (uint32_t)ty->size |
                  (hfa_kind << NYIR_ARG_AGG_CLASS0_SHIFT);
    return true;
  }
  unsigned c0 = NYIR_ARG_CLASS_INTEGER;
  unsigned c1 = NYIR_ARG_CLASS_NONE;
  if (ty->size > 16)
    c0 = NYIR_ARG_CLASS_MEMORY;
  else {
    if (ty->align > 8)
      c0 = NYIR_ARG_CLASS_AAPCS_INTEGER_A16;
    if (ty->size > 8)
      c1 = NYIR_ARG_CLASS_INTEGER;
  }
  *packed_out = (uint32_t)ty->size |
                (c0 << NYIR_ARG_AGG_CLASS0_SHIFT) |
                (c1 << NYIR_ARG_AGG_CLASS1_SHIFT);
  return true;
}

/*
 * Win64 passes 1, 2, 4, and 8 byte aggregates as one integer-class argument.
 * Other aggregate arguments are passed indirectly, and aggregate returns of
 * those sizes use the hidden result pointer.  The current C frontend's
 * aggregate field model lays out supported fields in 8-byte units, so the
 * only direct shape it can currently form is an 8-byte aggregate.
 */
static bool c_abi_win64_classify(const c_agg_type_t *ty, unsigned depth,
                                 uint32_t *packed_out) {
  if (!ty || !packed_out || !ty->complete || ty->size == 0 ||
      depth > C_AGG_MAX_DEPTH || ty->size > NYIR_ARG_AGG_SIZE_MASK)
    return false;
  unsigned cls = ty->size <= 8 ? NYIR_ARG_CLASS_INTEGER
                               : NYIR_ARG_CLASS_MEMORY;
  *packed_out = (uint32_t)ty->size |
                (cls << NYIR_ARG_AGG_CLASS0_SHIFT);
  return true;
}

static void c_abi_merge_class(unsigned *dst, unsigned src) {
  if (*dst == NYIR_ARG_CLASS_NONE)
    *dst = src;
  else if (*dst != src)
    *dst = NYIR_ARG_CLASS_INTEGER;
}

/*
 * Compute the eightbyte classes of one field relative to its own start.
 * Supported field shapes contribute clean, 8-byte-aligned class runs:
 * 8-byte scalars contribute a single class, nested aggregates contribute
 * their recursively computed one-or-two classes, and arrays repeat the
 * element classes across the array extent.
 */
static bool c_abi_field_profile(const c_agg_field_t *field, unsigned depth,
                                unsigned classes[2], size_t *classes_len) {
  classes[0] = NYIR_ARG_CLASS_NONE;
  classes[1] = NYIR_ARG_CLASS_NONE;
  *classes_len = 0;
  if (field->size == 0 || field->offset % 8 != 0 || field->size % 8 != 0)
    return false;
  switch (field->kind) {
  case C_AGG_FIELD_I64:
  case C_AGG_FIELD_PTR:
    classes[0] = NYIR_ARG_CLASS_INTEGER;
    *classes_len = 1;
    return true;
  case C_AGG_FIELD_F64:
    classes[0] = NYIR_ARG_CLASS_SSE;
    *classes_len = 1;
    return true;
  case C_AGG_FIELD_AGG: {
    uint32_t packed = 0;
    if (!nyc_sysv_classify_aggregate(field->agg, depth + 1, &packed))
      return false;
    if (nyc_sysv_return_is_memory(packed))
      return false;
    classes[0] = (packed >> NYIR_ARG_AGG_CLASS0_SHIFT) & NYIR_ARG_AGG_CLASS_MASK;
    classes[1] = (packed >> NYIR_ARG_AGG_CLASS1_SHIFT) & NYIR_ARG_AGG_CLASS_MASK;
    *classes_len = (field->size + 7u) / 8u;
    return *classes_len >= 1 && *classes_len <= 2;
  }
  case C_AGG_FIELD_ARRAY: {
    if (field->elem_count == 0)
      return false;
    size_t elem = field->size / field->elem_count;
    if (elem == 0 || field->size % elem != 0 || elem % 8 != 0)
      return false;
    unsigned elem_classes[2] = {NYIR_ARG_CLASS_NONE, NYIR_ARG_CLASS_NONE};
    size_t elem_pieces = elem / 8u;
    if (field->agg) {
      uint32_t packed = 0;
      if (!nyc_sysv_classify_aggregate(field->agg, depth + 1, &packed))
        return false;
      if (nyc_sysv_return_is_memory(packed))
        return false;
      elem_classes[0] =
          (packed >> NYIR_ARG_AGG_CLASS0_SHIFT) & NYIR_ARG_AGG_CLASS_MASK;
      elem_classes[1] =
          (packed >> NYIR_ARG_AGG_CLASS1_SHIFT) & NYIR_ARG_AGG_CLASS_MASK;
      if (elem_pieces != ((elem_classes[1] != NYIR_ARG_CLASS_NONE) ? 2u : 1u))
        return false;
    } else {
      /*
       * Scalar arrays: every element is an 8-byte INTEGER or SSE word.
       */
      elem_classes[0] =
          field->elem_f64 ? NYIR_ARG_CLASS_SSE : NYIR_ARG_CLASS_INTEGER;
      elem_pieces = 1;
    }
    size_t pieces = field->size / 8u;
    if (pieces < 1 || pieces > 2)
      return false;
    for (size_t piece = 0; piece < pieces; ++piece)
      classes[piece] = elem_classes[piece % elem_pieces];
    *classes_len = pieces;
    return true;
  }
  }
  return false;
}

bool nyc_sysv_classify_aggregate(const c_agg_type_t *ty, unsigned depth,
                                 uint32_t *packed_out) {
  if (!ty || !packed_out || !ty->complete || ty->size == 0 ||
      depth > C_AGG_MAX_DEPTH)
    return false;
  if (ty->size > 16) {
    *packed_out = (uint32_t)ty->size |
                  (NYIR_ARG_CLASS_MEMORY << NYIR_ARG_AGG_CLASS0_SHIFT);
    return true;
  }
  if (ty->size % 8 != 0)
    return false;
  unsigned classes[2] = {NYIR_ARG_CLASS_NONE, NYIR_ARG_CLASS_NONE};
  for (size_t i = 0; i < ty->field_count; ++i) {
    const c_agg_field_t *field = &ty->fields[i];
    if (field->offset + field->size > ty->size)
      return false;
    unsigned field_classes[2] = {NYIR_ARG_CLASS_NONE, NYIR_ARG_CLASS_NONE};
    size_t pieces = 0;
    if (!c_abi_field_profile(field, depth, field_classes, &pieces))
      return false;
    /*
     * Union members share offset 0; struct members pack in order. The
     * profile pieces are 8-byte aligned by construction.
     */
    size_t piece_offset = field->offset;
    for (size_t piece = 0; piece < pieces; ++piece) {
      size_t word = piece_offset / 8u;
      if (word > 1)
        return false;
      c_abi_merge_class(&classes[word], field_classes[piece]);
      piece_offset += 8;
    }
  }
  if (classes[0] == NYIR_ARG_CLASS_NONE)
    classes[0] = NYIR_ARG_CLASS_INTEGER;
  if (ty->size > 8 && classes[1] == NYIR_ARG_CLASS_NONE)
    classes[1] = NYIR_ARG_CLASS_INTEGER;
  *packed_out = ((uint32_t)ty->size & NYIR_ARG_AGG_SIZE_MASK) |
                (classes[0] << NYIR_ARG_AGG_CLASS0_SHIFT) |
                (classes[1] << NYIR_ARG_AGG_CLASS1_SHIFT);
  return true;
}

bool nyc_classify_aggregate(const c_agg_type_t *ty, nyc_abi_t abi,
                            unsigned depth, uint32_t *packed_out) {
  if (abi == NYC_ABI_AAPCS64)
    return c_abi_aapcs_classify(ty, depth, packed_out);
  if (abi == NYC_ABI_WIN64)
    return c_abi_win64_classify(ty, depth, packed_out);
  if (abi == NYC_ABI_SYSV_X86_64)
    return nyc_sysv_classify_aggregate(ty, depth, packed_out);
  if (abi == NYC_ABI_UNSUPPORTED)
    return false;
  return false;
}
