/*
 * Native lowering: translates compiler IR into NYIR, selecting
 * operand widths, applying ABI constraints, and emitting NYIR ops.
 *
 * Large lowering regions are kept in focused include fragments. This preserves
 * declaration order and a single translation unit while separating special/asm,
 * arithmetic, call, expression, and statement lowering for review and testing.
 */
#include "code/native/internal.h"
#include "code/native/ir/internal.h"
#include "code/c/c.h"
#include "code/priv.h"
#include "base/common.h"
#include "base/time.h"
#include "base/util.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/*
 * AST-to-NYIR lowering, extern discovery, optimized construction, dumps, and
 * metadata summaries. Execution and target emission live in other modules.
 *
 * Unlike the LLVM codegen layer (src/code/binary.c), the native pipeline
 * carries no tagged-int/BigInt fast-vs-slow dispatch.  NYIR integer values
 * are raw i64s and every supported binary op maps 1:1 to a raw NYIR
 * instruction (NYIR_ADD_I64 … NYIR_SAR_I64) with no runtime tag guard.
 * Unsupported dynamic shapes fail with a native diagnostic rather than
 * silently degrading to a runtime guard.
 *
 * Known slow-path fallbacks that still hand off to a slower form:
 *   - machine-form encode (ny_native_x86_64_emit_mach_scalar) → NYIR-object
 *     encoder (see ny_native_stat_nir_fallback),
 *   - register-color allocators (fp_fast_path / vec_fast_path) → stack spill
 *     (see ny_native_stat_regalloc_spilled),
 *   - tiered plan → AST interpretation
 *
 * Bounds-check elision in --safe-mode for Fin-typed indices is implemented:
 * ny_native_parse_fin_bound parses Fin<N> from parameter type annotations,
 * ny_native_nir_index_fin_bound_elision matches a Fin-indexed access against
 * the buffer's comptime byte length (tbuf_new provenance via
 * ny_native_extract_tbuf_new_len), and drops the NYIR_BOUNDS_CHECK when the
 * bound covers the access.  load64/store64/f64buf_load/store call it.
 *     (NY_NATIVE_CAP_AST_FALLBACK / prefer_ast_fallback).
 */

static bool ny_native_nir_binop(const char *op, nyir_op_t *out) {
  if (!op || !out)
    return false;
  if (strcmp(op, "+") == 0)
    *out = NYIR_ADD_I64;
  else if (strcmp(op, "-") == 0)
    *out = NYIR_SUB_I64;
  else if (strcmp(op, "*") == 0)
    *out = NYIR_MUL_I64;
  else if (strcmp(op, "/") == 0)
    *out = NYIR_DIV_I64;
  else if (strcmp(op, "%") == 0)
    *out = NYIR_MOD_I64;
  else if (strcmp(op, "&") == 0)
    *out = NYIR_AND_I64;
  else if (strcmp(op, "|") == 0)
    *out = NYIR_OR_I64;
  else if (strcmp(op, "^^") == 0)
    *out = NYIR_XOR_I64;
  else if (strcmp(op, "<<") == 0)
    *out = NYIR_SHL_I64;
  else if (strcmp(op, ">>") == 0)
    *out = NYIR_SAR_I64;
  else
    return false;
  return true;
}

/*
 * Native NYIR has no general power instruction. Keep the supported constant
 * subset in the language IR: exponentiation by a non-negative integer literal
 * is deterministic, has no runtime helper dependency, and can be represented
 * by one raw integer constant. Overflow remains an explicit lowering failure
 * instead of inheriting host-C signed-overflow behaviour.
 */
static bool ny_native_nir_fold_const_pow(const expr_t *left,
                                         const expr_t *right,
                                         int64_t *out) {
  if (!left || !right || !out || left->kind != NY_E_LITERAL ||
      right->kind != NY_E_LITERAL || left->as.literal.kind != NY_LIT_INT ||
      right->as.literal.kind != NY_LIT_INT || left->tok.kind == NY_T_NIL ||
      right->tok.kind == NY_T_NIL || right->as.literal.as.i < 0)
    return false;
  int64_t result = 1;
  int64_t base = left->as.literal.as.i;
  uint64_t exponent = (uint64_t)right->as.literal.as.i;
  while (exponent) {
    if ((exponent & 1u) && __builtin_mul_overflow(result, base, &result))
      return false;
    exponent >>= 1u;
    if (exponent && __builtin_mul_overflow(base, base, &base))
      return false;
  }
  *out = result;
  return true;
}

static bool ny_native_nir_cmp(const char *op, nyir_cmp_t *out) {
  if (!op || !out)
    return false;
  if (strcmp(op, "==") == 0)
    *out = NYIR_CMP_EQ;
  else if (strcmp(op, "!=") == 0)
    *out = NYIR_CMP_NE;
  else if (strcmp(op, "<") == 0)
    *out = NYIR_CMP_LT;
  else if (strcmp(op, "<=") == 0)
    *out = NYIR_CMP_LE;
  else if (strcmp(op, ">") == 0)
    *out = NYIR_CMP_GT;
  else if (strcmp(op, ">=") == 0)
    *out = NYIR_CMP_GE;
  else
    return false;
  return true;
}

static const char *ny_native_leaf_name(const char *name) {
  if (!name)
    return NULL;
  const char *dot = strrchr(name, '.');
  return dot ? dot + 1 : name;
}

static const char *ny_native_call_leaf(const expr_t *e) {
  if (!e || e->kind != NY_E_CALL || !e->as.call.callee)
    return NULL;
  if (e->as.call.callee->kind == NY_E_IDENT)
    return ny_native_leaf_name(e->as.call.callee->as.ident.name);
  if (e->as.call.callee->kind == NY_E_MEMBER)
    return ny_native_leaf_name(e->as.call.callee->as.member.name);
  return NULL;
}
typedef enum {
  NY_NATIVE_LEAF_NONE = 0,
  NY_NATIVE_LEAF_INTRINSIC,
  NY_NATIVE_LEAF_ASSERT,
  NY_NATIVE_LEAF_PRINT,
  NY_NATIVE_LEAF_FLOAT,
  NY_NATIVE_LEAF_ARGC,
  NY_NATIVE_LEAF_TICKS,
  NY_NATIVE_LEAF_FLT_SQRT,
  NY_NATIVE_LEAF_ADDR,
  NY_NATIVE_LEAF_F64BUF_NEW,
  NY_NATIVE_LEAF_F64BUF_LOAD,
  NY_NATIVE_LEAF_F64BUF_STORE,
  NY_NATIVE_LEAF_I64BUF_NEW,
  NY_NATIVE_LEAF_I64BUF_LOAD,
  NY_NATIVE_LEAF_I64BUF_STORE,
  NY_NATIVE_LEAF_LOAD8,
  NY_NATIVE_LEAF_STORE8,
  NY_NATIVE_LEAF_LOAD64,
  NY_NATIVE_LEAF_LOAD64_IDX,
  NY_NATIVE_LEAF_STORE64,
  NY_NATIVE_LEAF_STORE64_H,
  NY_NATIVE_LEAF_STORE64_IDX,
  NY_NATIVE_LEAF_IS_STR,
} ny_native_leaf_kind_t;

static ny_native_leaf_kind_t ny_native_leaf_kind(const char *leaf) {
  static const struct {
    const char *name;
    ny_native_leaf_kind_t kind;
  } names[] = {
      {"intrinsic", NY_NATIVE_LEAF_INTRINSIC},
      {"assert", NY_NATIVE_LEAF_ASSERT},
      {"print", NY_NATIVE_LEAF_PRINT},
      {"float", NY_NATIVE_LEAF_FLOAT},
      {"__argc", NY_NATIVE_LEAF_ARGC},
      {"argc", NY_NATIVE_LEAF_ARGC},
      {"ticks", NY_NATIVE_LEAF_TICKS},
      {"__flt_sqrt", NY_NATIVE_LEAF_FLT_SQRT},
      {"addr_of", NY_NATIVE_LEAF_ADDR},
      {"borrow", NY_NATIVE_LEAF_ADDR},
      {"f64buf_new", NY_NATIVE_LEAF_F64BUF_NEW},
      {"f64buf_load", NY_NATIVE_LEAF_F64BUF_LOAD},
      {"f64buf_store", NY_NATIVE_LEAF_F64BUF_STORE},
      {"i64buf_new", NY_NATIVE_LEAF_I64BUF_NEW},
      {"i64buf_load", NY_NATIVE_LEAF_I64BUF_LOAD},
      {"i64buf_store", NY_NATIVE_LEAF_I64BUF_STORE},
      {"load8", NY_NATIVE_LEAF_LOAD8},
      {"__load8_idx", NY_NATIVE_LEAF_LOAD8},
      {"store8", NY_NATIVE_LEAF_STORE8},
      {"__store8_idx", NY_NATIVE_LEAF_STORE8},
      {"load64_i", NY_NATIVE_LEAF_LOAD64},
      {"load64_h", NY_NATIVE_LEAF_LOAD64},
      {"load64", NY_NATIVE_LEAF_LOAD64},
      {"__load64_h", NY_NATIVE_LEAF_LOAD64},
      {"__load64_idx", NY_NATIVE_LEAF_LOAD64_IDX},
      {"store64_i", NY_NATIVE_LEAF_STORE64},
      {"store64_h", NY_NATIVE_LEAF_STORE64_H},
      {"store64", NY_NATIVE_LEAF_STORE64},
      {"__store64_h", NY_NATIVE_LEAF_STORE64_H},
      {"__store64_idx", NY_NATIVE_LEAF_STORE64_IDX},
      {"is_str", NY_NATIVE_LEAF_IS_STR},
      {"__is_str_obj", NY_NATIVE_LEAF_IS_STR},
  };
  if (!leaf)
    return NY_NATIVE_LEAF_NONE;
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
    if (strcmp(leaf, names[i].name) == 0)
      return names[i].kind;
  return NY_NATIVE_LEAF_NONE;
}
/*
 * Runtime builtins are exported by their C implementation names in
 * src/rt/defs.h.  Native NYIR calls use raw i64 values, matching the runtime
 * ABI for these helpers; keep the mapping generated from the single runtime
 * definition table instead of maintaining a second list here.
 */
static const char *ny_native_runtime_symbol(const char *name) {
  if (!name || !*name)
    return NULL;
  /*
   * Direct NYIR carries unboxed scalar values.  Runtime entry points whose
   * public ABI boxes ints/floats therefore need the native bridge rather than
   * the tagged interpreter/LLVM entry point.
   */
  if (strcmp(name, "__has_tag") == 0)
    return "rt_native_has_tag";
  if (strcmp(name, "__is_int") == 0)
    return "rt_native_is_int";
  if (strcmp(name, "__bigint_from_int") == 0)
    return "rt_bigint_from_i64_raw";
  if (strcmp(name, "__bigint_to_int") == 0)
    return "rt_bigint_to_i64_raw";
  if (strcmp(name, "__bigint_cmp") == 0)
    return "rt_native_bigint_cmp";
  if (strcmp(name, "__bigfloat_from_value") == 0)
    return "rt_native_bigfloat_from_value";
  if (strcmp(name, "__bigfloat_to_f64") == 0)
    return "rt_native_bigfloat_to_f64";
  if (strcmp(name, "__bigfloat_cmp") == 0)
    return "rt_native_bigfloat_cmp";
  if (strcmp(name, "__bigfloat_precision") == 0)
    return "rt_native_bigfloat_precision";
  if (strcmp(name, "__bigfloat_pow_int") == 0)
    return "rt_native_bigfloat_pow_int";
#define RT_DEF(n, implementation, args, sig, doc) \
  if (strcmp(name, n) == 0) return #implementation;
#define RT_GV(n, implementation, type, doc)
#include "rt/defs.h"
#undef RT_GV
#undef RT_DEF
  return NULL;
}
static bool ny_native_ffi_symbol_name(const char *name) {
  if (!name || !*name)
    return false;
  return strncmp(name, "vk", 2) == 0 ||
         strncmp(name, "gl", 2) == 0 ||
         strncmp(name, "egl", 3) == 0 ||
         strncmp(name, "X", 1) == 0 ||
         strncmp(name, "wl_", 3) == 0 ||
         strncmp(name, "xcb_", 4) == 0 ||
         strncmp(name, "FT_", 3) == 0;
}

typedef struct {
  const char *name;
  int slot;
  bool is_f64;
  bool is_f32;
  bool is_cstr;
  bool is_bool; /* raw 0/1 i64 from a comparison/logical/boolean literal */
  bool is_sb;   /* cstr backed by an amortized-growth builder (self-concat) */
  bool sb_candidate; /* unobserved mutable local initialized from a literal */
  int sb_slot;  /* slot holding the native builder handle, or -1 */
  bool is_any; /* any-typed: runtime tag dispatch required, not raw i64 */
  bool is_list; /* shared native list value: pointer slot + length slot */
  bool is_dyn_list; /* stride 24 (descriptors) vs stride 8 (scalars) */
  const expr_t *list_literal; /* current declaration's non-escaping literal */
  int list_len_slot;
  int dyn_str_len_slot;
  int dyn_tag_slot;
  int arg_slot;                /* incoming argument index for this parameter */
  int list_len_arg_slot;       /* incoming argument index for list length */
  int dyn_str_len_arg_slot;    /* incoming argument index for str length */
  int dyn_tag_arg_slot;        /* incoming argument index for str/any tag */
  int64_t buffer_byte_len; /* comptime-known allocation size in bytes, or 0 */
  int64_t fin_bound;       /* literal Fin<N> bound, or 0 when not known */
} ny_native_nir_local_t;

typedef struct {
  int head_label;
  int continue_label;
  int end_label;
  size_t defer_mark;
} ny_native_nir_loop_frame_t;

typedef enum {
  NY_NATIVE_NIR_FACT_ALLOC = 1,
  NY_NATIVE_NIR_FACT_FIN = 2,
  NY_NATIVE_NIR_FACT_LIST_LEN = 3,
  NY_NATIVE_NIR_FACT_DYN_STR_LEN = 4,
  NY_NATIVE_NIR_FACT_DYN_TAG = 5,
} ny_native_nir_fact_kind_t;

typedef struct {
  int value;
  int64_t payload;
  ny_native_nir_fact_kind_t kind;
} ny_native_nir_fact_t;

#define NY_EXTERN_MAX 1024

typedef enum {
  NY_SYSV_AGG_NONE = 0,
  NY_SYSV_AGG_INTEGER,
  NY_SYSV_AGG_SSE,
  NY_SYSV_AGG_MEMORY,
  NY_SYSV_AGG_UNSUPPORTED,
  NY_SYSV_AGG_HFA_F32,
  NY_SYSV_AGG_HFA_F64,
  NY_SYSV_AGG_HVA_V128,
  NY_SYSV_AGG_AAPCS_INTEGER_A16,
} ny_sysv_agg_class_t;

typedef struct {
  const char *ny_name;
  const char *c_symbol;
  unsigned param_count;
  bool owned;
  /*
   * Non-zero if the function returns an aggregate by value.
   */
  uint32_t ret_aggregate_size;
  ny_sysv_agg_class_t ret_aggregate_classes[2];
  /*
   * Per-argument byval sizes; 0 = scalar, >0 = aggregate of that byte size.
   */
  uint32_t arg_aggregate_sizes[NY_C_MAX_PARAMS];
} ny_extern_entry_t;

typedef struct {
  ny_extern_entry_t entries[NY_EXTERN_MAX];
  size_t count;
} ny_extern_table_t;

static void ny_extern_table_init(ny_extern_table_t *t) {
  if (t)
    t->count = 0;
}

static void ny_extern_table_free(ny_extern_table_t *t) {
  if (!t)
    return;
  for (size_t i = 0; i < t->count; ++i) {
    if (t->entries[i].owned) {
      free((void *)t->entries[i].ny_name);
      free((void *)t->entries[i].c_symbol);
    }
  }
  t->count = 0;
}

static bool ny_native_c_token_equal(ny_ctok_t a, ny_ctok_t b) {
  return a.kind == NY_CTOK_IDENT && b.kind == NY_CTOK_IDENT &&
         a.len == b.len && a.start && b.start &&
         memcmp(a.start, b.start, a.len) == 0;
}

static const ny_ctype_t *
ny_native_c_nested_type(const ny_parser_t *parser, const ny_c_field_t *field) {
  if (!parser || !field || field->type_name.kind != NY_CTOK_IDENT)
    return NULL;
  if (field->kind == NY_CTYPE_NAMED) {
    for (unsigned i = parser->typedef_count; i > 0; --i)
      if (ny_native_c_token_equal(parser->typedef_names[i - 1],
                                  field->type_name))
        return &parser->typedef_types[i - 1];
  }
  if (field->kind == NY_CTYPE_STRUCT || field->kind == NY_CTYPE_UNION) {
    for (unsigned i = parser->tag_count; i > 0; --i)
      if (parser->tag_types[i - 1].kind == field->kind &&
          ny_native_c_token_equal(parser->tag_names[i - 1],
                                  field->type_name))
        return &parser->tag_types[i - 1];
  }
  return NULL;
}

static void ny_native_sysv_merge_class(ny_sysv_agg_class_t *dst,
                                       ny_sysv_agg_class_t src) {
  if (*dst == NY_SYSV_AGG_NONE)
    *dst = src;
  else if (*dst != src)
    *dst = NY_SYSV_AGG_INTEGER;
}

static bool ny_native_sysv_classify_aggregate_depth(
    const ny_parser_t *parser, const ny_ctype_t *ty,
    ny_sysv_agg_class_t classes[2], unsigned depth) {
  classes[0] = NY_SYSV_AGG_NONE;
  classes[1] = NY_SYSV_AGG_NONE;
  if (!ty || !ty->aggregate_has_layout || ty->aggregate_size == 0 || depth > 8)
    return false;
  if (ty->aggregate_size > 16) {
    classes[0] = NY_SYSV_AGG_MEMORY;
    return true;
  }
  for (unsigned i = 0; i < ty->field_count; ++i) {
    const ny_c_field_t *field = &ty->fields[i];
    if ((field->align > 1 && field->offset % field->align != 0) ||
        field->offset + field->size > ty->aggregate_size) {
      classes[0] = NY_SYSV_AGG_MEMORY;
      classes[1] = NY_SYSV_AGG_NONE;
      return true;
    }
    ny_sysv_agg_class_t field_classes[2] = {NY_SYSV_AGG_INTEGER,
                                            NY_SYSV_AGG_NONE};
    if (field->ptr_depth == 0 &&
        (field->kind == NY_CTYPE_FLOAT || field->kind == NY_CTYPE_DOUBLE)) {
      field_classes[0] = NY_SYSV_AGG_SSE;
      if (field->size > 8)
        field_classes[1] = NY_SYSV_AGG_SSE;
    }
    else if (field->ptr_depth == 0 &&
             field->kind == NY_CTYPE_LONG_DOUBLE)
      return false;
    else if (field->ptr_depth == 0 &&
             (field->kind == NY_CTYPE_STRUCT ||
              field->kind == NY_CTYPE_UNION ||
              field->kind == NY_CTYPE_NAMED)) {
      const ny_ctype_t *nested = ny_native_c_nested_type(parser, field);
      if (!nested || !ny_native_sysv_classify_aggregate_depth(
                         parser, nested, field_classes, depth + 1))
        return false;
      if (field_classes[0] == NY_SYSV_AGG_MEMORY)
        return false;
    }
    size_t field_remaining = field->size;
    for (size_t nested_chunk = 0;
         nested_chunk < 2 && field_remaining > 0; ++nested_chunk) {
      ny_sysv_agg_class_t field_class = field_classes[nested_chunk];
      size_t nested_bytes = field_remaining > 8 ? 8 : field_remaining;
      size_t start = field->offset + nested_chunk * 8;
      size_t end = start + nested_bytes - 1;
      if (field_class == NY_SYSV_AGG_NONE || end / 8 > 1)
        return false;
      for (size_t chunk = start / 8; chunk <= end / 8; ++chunk)
        ny_native_sysv_merge_class(&classes[chunk], field_class);
      field_remaining -= nested_bytes;
    }
  }
  if (classes[0] == NY_SYSV_AGG_NONE)
    classes[0] = NY_SYSV_AGG_INTEGER;
  if (ty->aggregate_size > 8 && classes[1] == NY_SYSV_AGG_NONE)
    classes[1] = NY_SYSV_AGG_INTEGER;
  return true;
}

static bool ny_native_sysv_classify_aggregate(
    const ny_parser_t *parser, const ny_ctype_t *ty,
    ny_sysv_agg_class_t classes[2]) {
  return ny_native_sysv_classify_aggregate_depth(parser, ty, classes, 0);
}

static bool ny_native_aapcs_hfa_type(const ny_parser_t *parser,
                                        const ny_ctype_t *ty,
                                        ny_sysv_agg_class_t *kind,
                                        unsigned *count, unsigned depth) {
  if (!ty || !kind || !count || depth > 8 || ty->ptr_depth)
    return false;
  if (ty->kind == NY_CTYPE_FLOAT || ty->kind == NY_CTYPE_DOUBLE ||
      ty->kind == NY_CTYPE_HALF) {
    ny_sysv_agg_class_t k = ty->kind == NY_CTYPE_FLOAT ? NY_SYSV_AGG_HFA_F32
                            : ty->kind == NY_CTYPE_HALF ? NY_SYSV_AGG_HFA_F32
                                                        : NY_SYSV_AGG_HFA_F64;
    unsigned n = ty->array_elems ? (unsigned)ty->array_elems : 1u;
    if (n == 0 || n > 4)
      return false;
    *kind = k;
    *count = n;
    return true;
  }
  if (ty->kind == NY_CTYPE_NAMED && ty->name.kind == NY_CTOK_IDENT) {
    for (unsigned i = parser ? parser->typedef_count : 0; i > 0; --i) {
      if (ny_native_c_token_equal(parser->typedef_names[i - 1], ty->name))
        return ny_native_aapcs_hfa_type(parser, &parser->typedef_types[i - 1],
                                        kind, count, depth + 1);
    }
  }
  if ((ty->kind != NY_CTYPE_STRUCT && ty->kind != NY_CTYPE_NAMED) ||
      !ty->aggregate_has_layout || !ty->aggregate_size || !ty->field_count)
    return false;
  ny_sysv_agg_class_t aggregate_kind = NY_SYSV_AGG_NONE;
  unsigned aggregate_count = 0;
  size_t expected_offset = 0;
  for (unsigned i = 0; i < ty->field_count; ++i) {
    const ny_c_field_t *field = &ty->fields[i];
    if (field->ptr_depth || field->kind == NY_CTYPE_UNION)
      return false;
    ny_sysv_agg_class_t field_kind = NY_SYSV_AGG_NONE;
    unsigned field_count = 0;
    size_t elem_size = 0;
    if (field->kind == NY_CTYPE_FLOAT || field->kind == NY_CTYPE_DOUBLE ||
        field->kind == NY_CTYPE_HALF) {
      field_kind = field->kind == NY_CTYPE_FLOAT  ? NY_SYSV_AGG_HFA_F32
                   : field->kind == NY_CTYPE_HALF ? NY_SYSV_AGG_HFA_F32
                                                  : NY_SYSV_AGG_HFA_F64;
      elem_size = field->kind == NY_CTYPE_FLOAT ? 4u
                  : field->kind == NY_CTYPE_HALF ? 2u : 8u;
      if (!field->size || field->size % elem_size)
        return false;
      field_count = (unsigned)(field->size / elem_size);
    } else if (field->kind == NY_CTYPE_STRUCT ||
               field->kind == NY_CTYPE_NAMED) {
      const ny_ctype_t *nested = ny_native_c_nested_type(parser, field);
      if (!nested || !ny_native_aapcs_hfa_type(parser, nested, &field_kind,
                                               &field_count, depth + 1))
        return false;
      elem_size = field_kind == NY_SYSV_AGG_HFA_F32 ? 4u : 8u;
      size_t nested_size = nested->aggregate_size;
      if (!nested_size || !field->size || field->size % nested_size)
        return false;
      size_t repeat = field->size / nested_size;
      if (!repeat || repeat > 4 || field_count > 4 / repeat)
        return false;
      field_count *= (unsigned)repeat;
    } else {
      return false;
    }
    if (!field_count || field_count > 4 ||
        (aggregate_kind != NY_SYSV_AGG_NONE && aggregate_kind != field_kind) ||
        field->offset != expected_offset)
      return false;
    aggregate_kind = field_kind;
    aggregate_count += field_count;
    if (aggregate_count > 4)
      return false;
    expected_offset += (size_t)field_count * elem_size;
  }
  if (aggregate_kind == NY_SYSV_AGG_NONE || !aggregate_count ||
      expected_offset != ty->aggregate_size)
    return false;
  *kind = aggregate_kind;
  *count = aggregate_count;
  return true;
}

static bool ny_native_aapcs_classify_aggregate(
    const ny_parser_t *parser, const ny_ctype_t *ty,
    ny_sysv_agg_class_t classes[2]) {
  classes[0] = NY_SYSV_AGG_NONE;
  classes[1] = NY_SYSV_AGG_NONE;
  if (!ty || !ty->aggregate_has_layout || !ty->aggregate_size)
    return false;
  ny_sysv_agg_class_t hfa = NY_SYSV_AGG_NONE;
  unsigned count = 0;
  if (ny_native_aapcs_hfa_type(parser, ty, &hfa, &count, 0) &&
      count >= 1 && count <= 4) {
    classes[0] = hfa;
    return true;
  }
  if (ty->aggregate_size > 16) {
    classes[0] = NY_SYSV_AGG_MEMORY;
    return true;
  }
  classes[0] = ty->aggregate_align > 8
                   ? NY_SYSV_AGG_AAPCS_INTEGER_A16
                   : NY_SYSV_AGG_INTEGER;
  if (ty->aggregate_size > 8)
    classes[1] = NY_SYSV_AGG_INTEGER;
  return true;
}

static bool ny_extern_table_add(ny_extern_table_t *t, const char *ny_name,
                                const char *c_symbol, unsigned param_count,
                                bool owned, uint32_t ret_agg_size,
                                const ny_sysv_agg_class_t ret_agg_classes[2],
                                const uint32_t *arg_agg_sizes) {
  if (!t || !ny_name || !c_symbol)
    return false;
  /*
   * Dedup: identical redeclarations are silently accepted.
   */
  for (size_t i = 0; i < t->count; ++i) {
    if (t->entries[i].ny_name && strcmp(t->entries[i].ny_name, ny_name) == 0) {
      if (t->entries[i].c_symbol &&
          strcmp(t->entries[i].c_symbol, c_symbol) == 0) {
        if (owned) {
          free((void *)ny_name);
          free((void *)c_symbol);
        }
        return true; /* exact duplicate — ok */
      }
      return false; /* conflicting extern: same NY name, different C symbol */
    }
  }
  if (t->count >= NY_EXTERN_MAX) {
    fprintf(stderr, "native NYIR lower: extern table full (%zu entries)\n", t->count);
    return false;
  }
  t->entries[t->count].ny_name = ny_name;
  t->entries[t->count].c_symbol = c_symbol;
  t->entries[t->count].param_count = param_count;
  t->entries[t->count].owned = owned;
  t->entries[t->count].ret_aggregate_size = ret_agg_size;
  t->entries[t->count].ret_aggregate_classes[0] =
      ret_agg_classes ? ret_agg_classes[0] : NY_SYSV_AGG_NONE;
  t->entries[t->count].ret_aggregate_classes[1] =
      ret_agg_classes ? ret_agg_classes[1] : NY_SYSV_AGG_NONE;
  memset(t->entries[t->count].arg_aggregate_sizes, 0,
         sizeof(t->entries[t->count].arg_aggregate_sizes));
  if (arg_agg_sizes && param_count > 0) {
    size_t n = param_count < NY_C_MAX_PARAMS ? param_count : NY_C_MAX_PARAMS;
    for (size_t k = 0; k < n; ++k)
      t->entries[t->count].arg_aggregate_sizes[k] = arg_agg_sizes[k];
  }
  t->count++;
  return true;
}

static const ny_extern_entry_t *ny_extern_table_lookup(
    const ny_extern_table_t *t, const char *ny_name) {
  if (!t || !ny_name)
    return NULL;
  for (size_t i = 0; i < t->count; ++i) {
    if (t->entries[i].ny_name &&
        strcmp(t->entries[i].ny_name, ny_name) == 0)
      return &t->entries[i];
  }
  return NULL;
}

typedef struct {
  nyir_func_t nyir;
  ny_native_nir_local_t *locals;
  size_t local_count;
  size_t local_cap;
  int next_local_slot;
  int next_label;
  int last_value;
  int resolve_depth;
  ny_native_nir_fact_t *facts;
  size_t fact_count;
  size_t fact_cap;
  ny_native_nir_loop_frame_t *loop_frames;
  size_t loop_depth;
  size_t loop_cap;
  stmt_t **defers;
  size_t defer_count;
  size_t defer_cap;
  bool emitted_return;
  /*
   * Function return type is carried on NYIR_RET so type constraints survive
   * control-flow joins that contain no floating-point arithmetic themselves.
   */
  unsigned return_flags;
  const ny_extern_table_t *externs;
  const program_t *prog;
  const ny_options *options;
  const char *profile_name;
  const char *current_fn_name;
  int64_t current_list_elem_size;
  /*
   * Bounded self-tail recursion lowering.  These fields are enabled only for
   * scalar-parameter functions; aggregate ABI values remain ordinary calls.
   */
  int tail_loop_label;
  int *tail_param_slots;
  size_t tail_param_count;
  const stmt_t *tail_body;
  bool tail_recur_enabled;
  int opt_level;
  char *err;
  size_t err_len;
} ny_native_nir_builder_t;

/*
 * Parse a Fin<N> type name and extract the bound.
 * Handles "Fin<42>" (literal); "Fin<N>" symbolic names are not resolved here.
 */
static int64_t ny_native_parse_fin_bound(const char *type_name) {
  if (!type_name)
    return -1;
  const char *start = strstr(type_name, "Fin<");
  if (!start)
    return -1;
  start += 4;
  const char *end = strchr(start, '>');
  if (!end || end == start)
    return -1;
  char *check = NULL;
  long long val = strtoll(start, &check, 10);
  if (check != end || val < 0)
    return -1;
  return (int64_t)val;
}

/*
 * Check whether an intrinsic call's index operand has a Fin type whose
 * bound allows bounds-check elision.  buffer_byte_len must be > 0 (pass
 * the known allocation / tbuf capacity in bytes; 0 = unknown).
 * Checks both function parameter types and local variable type annotations.
 */
static ny_native_nir_local_t *ny_native_nir_find_local(
    ny_native_nir_builder_t *b, const char *name);

static int64_t ny_native_nir_fin_bound_for_name(
    const ny_native_nir_builder_t *b, const char *name, unsigned depth) {
  if (!b || !name || !name[0] || depth > 16)
    return 0;
  ny_native_nir_local_t *local =
      ny_native_nir_find_local((ny_native_nir_builder_t *)b, name);
  if (local && local->fin_bound > 0)
    return local->fin_bound;
  if (!b->prog)
    return 0;
  for (size_t i = 0; i < b->prog->body.len; ++i) {
    const stmt_t *s = b->prog->body.data[i];
    if (!s)
      continue;
    if (strcmp(b->current_fn_name ? b->current_fn_name : "", "rt_main") == 0 &&
        s->kind == NY_S_VAR) {
      for (size_t vi = 0; vi < s->as.var.names.len; ++vi) {
        if (!s->as.var.names.data[vi] ||
            strcmp(s->as.var.names.data[vi], name) != 0)
          continue;
        if (vi < s->as.var.types.len) {
          int64_t bound = ny_native_parse_fin_bound(s->as.var.types.data[vi]);
          if (bound > 0)
            return bound;
        }
        if (vi < s->as.var.exprs.len && s->as.var.exprs.data[vi] &&
            s->as.var.exprs.data[vi]->kind == NY_E_IDENT)
          return ny_native_nir_fin_bound_for_name(
              b, s->as.var.exprs.data[vi]->as.ident.name, depth + 1);
      }
    }
    if (s->kind != NY_S_FUNC || !s->as.fn.name ||
        strcmp(s->as.fn.name, b->current_fn_name ? b->current_fn_name : "") != 0)
      continue;
    for (size_t pi = 0; pi < s->as.fn.params.len; ++pi) {
      if (s->as.fn.params.data[pi].name &&
          strcmp(s->as.fn.params.data[pi].name, name) == 0)
        return ny_native_parse_fin_bound(s->as.fn.params.data[pi].type);
    }
    if (!s->as.fn.body || s->as.fn.body->kind != NY_S_BLOCK)
      continue;
    for (size_t si = 0; si < s->as.fn.body->as.block.body.len; ++si) {
      const stmt_t *vs = s->as.fn.body->as.block.body.data[si];
      if (!vs || vs->kind != NY_S_VAR)
        continue;
      for (size_t vi = 0; vi < vs->as.var.names.len; ++vi) {
        if (!vs->as.var.names.data[vi] ||
            strcmp(vs->as.var.names.data[vi], name) != 0)
          continue;
        if (vi < vs->as.var.types.len) {
          int64_t bound = ny_native_parse_fin_bound(vs->as.var.types.data[vi]);
          if (bound > 0)
            return bound;
        }
        if (vi < vs->as.var.exprs.len && vs->as.var.exprs.data[vi] &&
            vs->as.var.exprs.data[vi]->kind == NY_E_IDENT)
          return ny_native_nir_fin_bound_for_name(
              b, vs->as.var.exprs.data[vi]->as.ident.name, depth + 1);
      }
    }
  }
  return 0;
}

static bool ny_native_nir_index_fin_bound_elision(
    const ny_native_nir_builder_t *b, const expr_t *e, size_t index_arg_pos,
    int64_t buffer_byte_len) {
  if (!b || !b->current_fn_name || !e || buffer_byte_len <= 0 ||
      index_arg_pos >= e->as.call.args.len)
    return false;
  const expr_t *idx_expr = e->as.call.args.data[index_arg_pos].val;
  if (!idx_expr)
    return false;
  int64_t scale = 1;
  const char *idx_name = NULL;
  if (idx_expr->kind == NY_E_IDENT) {
    idx_name = idx_expr->as.ident.name;
  } else if (idx_expr->kind == NY_E_BINARY &&
             idx_expr->as.binary.op &&
             strcmp(idx_expr->as.binary.op, "*") == 0) {
    const expr_t *l = idx_expr->as.binary.left;
    const expr_t *r = idx_expr->as.binary.right;
    if (l && l->kind == NY_E_IDENT && r && r->kind == NY_E_LITERAL &&
        r->as.literal.kind == NY_LIT_INT && r->as.literal.as.i > 0) {
      idx_name = l->as.ident.name;
      scale = r->as.literal.as.i;
    } else if (r && r->kind == NY_E_IDENT && l && l->kind == NY_E_LITERAL &&
               l->as.literal.kind == NY_LIT_INT && l->as.literal.as.i > 0) {
      idx_name = r->as.ident.name;
      scale = l->as.literal.as.i;
    }
  }
  int64_t fin_bound =
      ny_native_nir_fin_bound_for_name(b, idx_name, 0);
  return fin_bound > 0 && fin_bound <= INT64_MAX / scale &&
         fin_bound * scale <= buffer_byte_len;
}

/*
 * Resolve the comptime buffer byte length from the first argument of a
 * tbuf intrinsic call (__load64_idx, f64buf_load, etc.).  Returns 0 if
 * the pointer argument isn't a named local with known buffer_byte_len.
 */
static int64_t ny_native_nir_resolve_buf_byte_len(
    const ny_native_nir_builder_t *b, const expr_t *e) {
  if (!b || !e || e->as.call.args.len == 0)
    return 0;
  const expr_t *ptr_arg = e->as.call.args.data[0].val;
  if (!ptr_arg || ptr_arg->kind != NY_E_IDENT)
    return 0;
  ny_native_nir_local_t *bl =
      ny_native_nir_find_local((ny_native_nir_builder_t *)b,
                               ptr_arg->as.ident.name);
  return (bl && bl->buffer_byte_len > 0) ? bl->buffer_byte_len : 0;
}

static bool ny_native_nir_fail(ny_native_nir_builder_t *b, const char *fmt,
                               ...) __attribute__((format(printf, 2, 3)));

static void ny_native_nir_builder_dispose(ny_native_nir_builder_t *b) {
  if (!b)
    return;
  free(b->locals);
  free(b->loop_frames);
  free(b->facts);
  free(b->defers);
  free(b->tail_param_slots);
  b->locals = NULL;
  b->loop_frames = NULL;
  b->facts = NULL;
  b->defers = NULL;
  b->tail_param_slots = NULL;
  b->local_count = 0;
  b->local_cap = 0;
  b->loop_depth = 0;
  b->loop_cap = 0;
  b->fact_count = 0;
  b->fact_cap = 0;
  b->defer_count = 0;
  b->defer_cap = 0;
  b->tail_param_count = 0;
}

static bool ny_native_nir_push_loop(ny_native_nir_builder_t *b, int head_label,
                                    int continue_label, int end_label) {
  if (!b)
    return false;
  if (b->loop_depth == b->loop_cap) {
    size_t cap = b->loop_cap ? b->loop_cap * 2 : 16;
    if (cap < b->loop_cap || cap > SIZE_MAX / sizeof(*b->loop_frames))
      return ny_native_nir_fail(b, "native NYIR lower: loop table is too large");
    ny_native_nir_loop_frame_t *frames =
        realloc(b->loop_frames, cap * sizeof(*frames));
    if (!frames)
      return ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
    b->loop_frames = frames;
    b->loop_cap = cap;
  }
  b->loop_frames[b->loop_depth++] = (ny_native_nir_loop_frame_t){
      .head_label = head_label,
      .continue_label = continue_label,
      .end_label = end_label,
      .defer_mark = b->defer_count,
  };
  return true;
}

static int ny_native_nir_temp_slot(ny_native_nir_builder_t *b) {
  return b ? b->next_local_slot++ : -1;
}

static size_t ny_native_nir_scope_mark(ny_native_nir_builder_t *b) {
  return b ? b->local_count : 0;
}

static void ny_native_nir_scope_restore(ny_native_nir_builder_t *b,
                                        size_t mark) {
  if (b && mark <= b->local_count)
    b->local_count = mark;
}

/*
 * Forward declarations (the statement lowerer is defined later in this file).
 */
static bool ny_native_nir_lower_stmt(ny_native_nir_builder_t *b, const stmt_t *s);

/*
 * Push a deferred body so it runs LIFO at the enclosing scope's exit.
 */
static bool ny_native_nir_push_defer(ny_native_nir_builder_t *b, stmt_t *body) {
  if (!b || !body)
    return true;
  if (b->defer_count == b->defer_cap) {
    size_t cap = b->defer_cap ? b->defer_cap * 2 : 16;
    if (cap < b->defer_cap || cap > SIZE_MAX / sizeof(*b->defers))
      return ny_native_nir_fail(b, "native NYIR lower: defer table is too large");
    stmt_t **defers = realloc(b->defers, cap * sizeof(*defers));
    if (!defers)
      return ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
    b->defers = defers;
    b->defer_cap = cap;
  }
  b->defers[b->defer_count++] = body;
  return true;
}

/*
 * Run deferred bodies registered since mark, LIFO, then drop them.
 */
static bool ny_native_nir_emit_defers(ny_native_nir_builder_t *b, size_t mark) {
  if (!b || b->defer_count <= mark)
    return true;
  size_t saved_return = b->emitted_return;
  while (b->defer_count > mark) {
    stmt_t *body = b->defers[--b->defer_count];
    if (body && !ny_native_nir_lower_stmt(b, body)) {
      b->emitted_return = saved_return;
      return false;
    }
    if (b->emitted_return) /* a defer body returned; stop unwinding */
      break;
  }
  b->emitted_return = saved_return;
  return true;
}

/*
 * Lower a statement body inside a synthetic scope: defers registered by the
 * body are emitted (LIFO) when the body finishes, so single-statement loop/if
 * bodies get per-iteration / per-branch defer semantics.  For block bodies
 * the block already emits its own defers at exit, so this is a no-op.
 * Returns the body's success; last_value is preserved across defer emission.
 */
static bool ny_native_nir_lower_scoped_body(ny_native_nir_builder_t *b,
                                            const stmt_t *body) {
  size_t defer_mark = b->defer_count;
  if (!ny_native_nir_lower_stmt(b, body))
    return false;
  if (b->emitted_return)
    return true; /* return/break/continue already ran the defers */
  int saved_last = b->last_value;
  if (!ny_native_nir_emit_defers(b, defer_mark))
    return false;
  b->last_value = saved_last;
  return true;
}

static bool ny_native_nir_fail(ny_native_nir_builder_t *b, const char *fmt, ...) {
  if (!b || !b->err || b->err_len == 0)
    return false;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b->err, b->err_len, fmt, ap);
  va_end(ap);
  return false;
}

static bool ny_native_nir_ignored_stmt(const stmt_t *s) {
  return !s || s->kind == NY_S_USE || s->kind == NY_S_LINK ||
         s->kind == NY_S_INCLUDE || s->kind == NY_S_DEFINE ||
         s->kind == NY_S_EXPORT || s->kind == NY_S_MODULE ||
         s->kind == NY_S_EXTERN;
}

static ny_native_nir_local_t *ny_native_nir_find_local(ny_native_nir_builder_t *b,
                                                       const char *name) {
  if (!b || !name)
    return NULL;
  for (size_t i = b->local_count; i > 0; --i) {
    ny_native_nir_local_t *l = &b->locals[i - 1];
    if (l->name && strcmp(l->name, name) == 0)
      return l;
  }
  return NULL;
}

/*
 * Forward declarations needed by ny_native_nir_expr_is_dyn_list which is
 * defined here but calls helpers defined later in the file.
 */
static bool ny_native_type_name_is_list(const char *name);
static bool ny_native_nir_expr_is_list(const ny_native_nir_builder_t *b,
                                       const expr_t *e);
static bool ny_native_nir_expr_is_f64(ny_native_nir_builder_t *b, const expr_t *e);
static bool ny_native_nir_expr_is_cstr(ny_native_nir_builder_t *b, const expr_t *e);
static bool ny_native_nir_expr_is_bool(ny_native_nir_builder_t *b, const expr_t *e);
static bool ny_native_nir_expr_is_any(ny_native_nir_builder_t *b, const expr_t *e);
static const expr_t *ny_native_nir_find_top_level_value(const ny_native_nir_builder_t *b, const char *name);
static const expr_t *ny_native_nir_resolve_member_expr(const ny_native_nir_builder_t *b, const expr_t *e);

static bool ny_native_type_name_is_dyn_list(const char *name) {
  if (!name || !ny_native_type_name_is_list(name))
    return false;
  return strstr(name, "<any>") || strstr(name, "<str>") ||
         strstr(name, "<string>") || strstr(name, "any[]") ||
         strstr(name, "str[]") || strstr(name, "string[]");
}

static bool ny_native_nir_function_param_is_dyn_list(const program_t *prog,
                                                     const stmt_t *fn,
                                                     size_t param_index);

static bool ny_native_nir_expr_is_dyn_list(ny_native_nir_builder_t *b,
                                           const expr_t *e) {
  if (!e)
    return false;
  if (e->kind == NY_E_LIST) {
    /*
     * An empty literal is the dynamic tbuf representation's initial value.
     * It has no element evidence, but it is still a 24-byte descriptor list.
     */
    if (e->as.list_like.len == 0)
      return true;
    bool has_f64 = false, has_int = false;
    for (size_t i = 0; i < e->as.list_like.len; ++i) {
      const expr_t *el = e->as.list_like.data[i];
      const expr_t *scalar = el;
      if (scalar && scalar->kind == NY_E_UNARY && scalar->as.unary.op &&
          (strcmp(scalar->as.unary.op, "+") == 0 ||
           strcmp(scalar->as.unary.op, "-") == 0))
        scalar = scalar->as.unary.right;
      if (!scalar || scalar->kind != NY_E_LITERAL)
        return true;
      if (ny_native_nir_expr_is_cstr(b, el) || ny_native_nir_expr_is_any(b, el))
        return true;
      if (ny_native_nir_expr_is_f64(b, el))
        has_f64 = true;
      else
        has_int = true;
    }
    return has_f64 && has_int;
  }
  if (e->kind == NY_E_BINARY && e->as.binary.op &&
      strcmp(e->as.binary.op, "*") == 0) {
    bool left_list = ny_native_nir_expr_is_list(b, e->as.binary.left);
    bool right_list = ny_native_nir_expr_is_list(b, e->as.binary.right);
    if (left_list != right_list)
      return ny_native_nir_expr_is_dyn_list(
          b, left_list ? e->as.binary.left : e->as.binary.right);
  }
  if (e->kind == NY_E_IDENT) {
    ny_native_nir_local_t *l = ny_native_nir_find_local(b, e->as.ident.name);
    if (l)
      return l->is_dyn_list;
    const expr_t *g = ny_native_nir_find_top_level_value(b, e->as.ident.name);
    if (g && g != e)
      return ny_native_nir_expr_is_dyn_list(b, g);
  }
  if (e->kind == NY_E_MEMBER) {
    const expr_t *v = ny_native_nir_resolve_member_expr(b, e);
    if (v && v != e)
      return ny_native_nir_expr_is_dyn_list(b, v);
  }
  return false;
}

static bool ny_native_type_name_is_f64(const char *name) {
  return name && (strcmp(name, "f64") == 0 || strcmp(name, "float") == 0 ||
                  strcmp(name, "number") == 0 || strcmp(name, "double") == 0);
}

static bool ny_native_type_name_is_f32(const char *name) {
  return name && (strcmp(name, "f32") == 0 || strcmp(name, "float32") == 0);
}

static bool ny_native_type_name_is_list(const char *name) {
  if (!name)
    return false;
  const char *leaf = strrchr(name, '.');
  leaf = leaf ? leaf + 1 : name;
  return strncmp(leaf, "list", 4) == 0 &&
         (leaf[4] == '\0' || leaf[4] == '<' || leaf[4] == '[' ||
          isspace((unsigned char)leaf[4]));
}

static bool ny_native_type_name_is_str(const char *name) {
  return name && (strcmp(name, "str") == 0 || strcmp(name, "string") == 0);
}

static bool ny_native_type_name_is_any(const char *name) {
  return name && strcmp(name, "any") == 0;
}

static bool ny_native_nir_set_param_types(ny_native_nir_builder_t *b,
                                               const stmt_t *fn) {
  if (!b || !fn || fn->kind != NY_S_FUNC)
    return false;
  size_t source_count = fn->as.fn.params.len;
  size_t count = source_count;
  for (size_t i = 0; i < source_count; ++i) {
    const char *type = fn->as.fn.params.data[i].type;
    if (ny_native_type_name_is_list(type))
      count++;
    else if (ny_native_type_name_is_str(type) || ny_native_type_name_is_any(type))
      count += 2;
  }
  if (!count)
    return true;
  b->nyir.param_types = calloc(count, sizeof(*b->nyir.param_types));
  if (!b->nyir.param_types)
    return ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
  b->nyir.param_count = count;
  size_t abi_index = 0;
  for (size_t i = 0; i < source_count; ++i) {
    const char *type = fn->as.fn.params.data[i].type;
    b->nyir.param_types[abi_index++] = ny_native_type_name_is_f64(type)
                                           ? NYIR_PARAM_F64
                                           : ny_native_type_name_is_f32(type)
                                                 ? NYIR_PARAM_F32
                                                 : NYIR_PARAM_I64;
    if (ny_native_type_name_is_list(type))
      b->nyir.param_types[abi_index++] = NYIR_PARAM_I64;
    else if (ny_native_type_name_is_str(type) || ny_native_type_name_is_any(type)) {
      b->nyir.param_types[abi_index++] = NYIR_PARAM_I64;
      b->nyir.param_types[abi_index++] = NYIR_PARAM_I64;
    }
  }
  return true;
}


static int64_t ny_native_f64_bits(double v) {
  return nyir_f64_to_bits(v);
}

static int64_t ny_native_f32_bits(float v) {
  uint32_t bits = 0;
  memcpy(&bits, &v, sizeof(bits));
  return (int64_t)bits;
}

static ny_native_nir_local_t *ny_native_nir_bind_local_typed(
    ny_native_nir_builder_t *b, const char *name, bool is_f64, bool is_f32,
    bool is_cstr) {
  if (!name || !name[0] || strcmp(name, "_") == 0)
    return NULL;
  if (b->local_count == b->local_cap) {
    size_t cap = b->local_cap ? b->local_cap * 2 : 64;
    if (cap < b->local_cap || cap > SIZE_MAX / sizeof(*b->locals)) {
      ny_native_nir_fail(b, "native NYIR lower: local table is too large");
      return NULL;
    }
    ny_native_nir_local_t *locals = realloc(b->locals, cap * sizeof(*locals));
    if (!locals) {
      ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
      return NULL;
    }
    b->locals = locals;
    b->local_cap = cap;
  }
  ny_native_nir_local_t *l = &b->locals[b->local_count];
  *l = (ny_native_nir_local_t){.name = name,
                               .slot = b->next_local_slot++,
                               .is_f64 = is_f64,
                               .is_f32 = is_f32,
                               .is_cstr = is_cstr,
                               .sb_slot = -1,
                               .list_literal = NULL,
                               .list_len_slot = -1,
                               .dyn_str_len_slot = -1,
                               .dyn_tag_slot = -1,
                               .arg_slot = -1,
                               .list_len_arg_slot = -1,
                               .dyn_str_len_arg_slot = -1,
                               .dyn_tag_arg_slot = -1};
  b->local_count++;
  return l;
}

static ny_native_nir_local_t *ny_native_nir_bind_local(ny_native_nir_builder_t *b,
                                                       const char *name) {
  return ny_native_nir_bind_local_typed(b, name, false, false, false);
}

static ny_native_nir_local_t *ny_native_nir_add_local(ny_native_nir_builder_t *b,
                                                      const char *name) {
  if (!name || !name[0] || strcmp(name, "_") == 0)
    return NULL;
  ny_native_nir_local_t *old = ny_native_nir_find_local(b, name);
  return old ? old : ny_native_nir_bind_local(b, name);
}

/*
 * Compact NYIR emit helpers: one push path for control/value forms.
 */
static bool ny_native_nir_push_ctrl(ny_native_nir_builder_t *b, nyir_op_t op,
                                    int a, int64_t imm) {
  size_t before = b->nyir.len;
  nyir_emit(&b->nyir, (nyir_inst_t){.op = op, .dst = -1, .a = a, .b = -1,
                                       .imm = imm});
  return b->nyir.len != before ||
         ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
}

static int ny_native_nir_push_val(ny_native_nir_builder_t *b, nyir_op_t op,
                                  int a, int b_arg, int64_t imm,
                                  const char *symbol) {
  int v = nyir_emit(&b->nyir, (nyir_inst_t){.op = op,
                                               .dst = -1,
                                               .a = a,
                                               .b = b_arg,
                                               .imm = imm,
                                               .symbol = symbol});
  if (v < 0)
    ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
  return v;
}

static bool ny_native_nir_emit_label(ny_native_nir_builder_t *b, int label) {
  return ny_native_nir_push_ctrl(b, NYIR_LABEL, -1, label);
}
static bool ny_native_nir_emit_br(ny_native_nir_builder_t *b, int label) {
  return ny_native_nir_push_ctrl(b, NYIR_BR, -1, label);
}
static bool ny_native_nir_emit_br_if(ny_native_nir_builder_t *b, int value,
                                     int label) {
  return ny_native_nir_push_ctrl(b, NYIR_BR_IF, value, label);
}

static bool ny_native_nir_emit_ret(ny_native_nir_builder_t *b, int value) {
  size_t before = b->nyir.len;
  nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_RET,
                                       .dst = -1,
                                       .a = value,
                                       .b = -1,
                                       .flags = b ? b->return_flags : 0});
  if (b->nyir.len == before)
    return ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
  b->emitted_return = true;
  b->last_value = value;
  return true;
}

static bool ny_native_nir_record_dyn_fact(ny_native_nir_builder_t *b,
                                          int value,
                                          ny_native_nir_fact_kind_t kind,
                                          int reg);
static bool ny_native_nir_record_list_len_fact(ny_native_nir_builder_t *b,
                                               int value, int length_value);
static int ny_native_nir_emit_add_i64(ny_native_nir_builder_t *b, int a,
                                      int rhs);
static bool ny_native_nir_emit_store_i64(ny_native_nir_builder_t *b, int addr,
                                         int value);
static int ny_native_nir_emit_runtime_call(ny_native_nir_builder_t *b,
                                           const char *symbol, int a, int b_arg,
                                           int c, int argc, unsigned flags);
static int ny_native_nir_emit_const(ny_native_nir_builder_t *b, int64_t value) {
  return ny_native_nir_push_val(b, NYIR_CONST_I64, -1, -1, value, NULL);
}
static int ny_native_nir_emit_cstr_const(ny_native_nir_builder_t *b,
                                         const char *s) {
  size_t len = s ? strlen(s) : 0;
  const char *sym = ny_native_strtab_intern(s ? s : "", len, NULL, 0);
  if (!sym) {
    ny_native_nir_fail(b, "native NYIR lower: string table full or OOM");
    return -1;
  }
  int addr = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_ADDR_SYMBOL,
                                                .dst = -1,
                                                .a = -1,
                                                .b = -1,
                                                .imm = 0,
                                                .symbol = sym});
  if (addr < 0) {
    ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
    return -1;
  }
  int length = ny_native_nir_emit_const(b, (int64_t)len);
  int tag = ny_native_nir_emit_const(b, 121);
  if (length >= 0 && tag >= 0) {
    ny_native_nir_record_dyn_fact(b, addr, NY_NATIVE_NIR_FACT_DYN_STR_LEN,
                                  length);
    ny_native_nir_record_dyn_fact(b, addr, NY_NATIVE_NIR_FACT_DYN_TAG, tag);
  }
  return addr;
}
static int ny_native_nir_emit_pair_list_i64(ny_native_nir_builder_t *b,
                                            int first, int second) {
  int count = ny_native_nir_emit_const(b, 2);
  int width = ny_native_nir_emit_const(b, 24);
  int base = count < 0 || width < 0
                 ? -1
                 : ny_native_nir_emit_runtime_call(
                       b, "rt_native_tbuf_new", count, width, -1, 2, 0);
  int zero = ny_native_nir_emit_const(b, 0);
  int int_tag = ny_native_nir_emit_const(b, 3);
  int stride = ny_native_nir_emit_const(b, 24);
  if (base < 0 || first < 0 || second < 0 || zero < 0 || int_tag < 0 ||
      stride < 0)
    return -1;
  for (int i = 0; i < 2; ++i) {
    int slot = i == 0 ? base : ny_native_nir_emit_add_i64(b, base, stride);
    int len_slot = slot < 0 ? -1
                            : ny_native_nir_emit_add_i64(
                                  b, slot, ny_native_nir_emit_const(b, 8));
    int tag_slot = slot < 0 ? -1
                            : ny_native_nir_emit_add_i64(
                                  b, slot, ny_native_nir_emit_const(b, 16));
    if (slot < 0 || len_slot < 0 || tag_slot < 0 ||
        !ny_native_nir_emit_store_i64(b, slot, i == 0 ? first : second) ||
        !ny_native_nir_emit_store_i64(b, len_slot, zero) ||
        !ny_native_nir_emit_store_i64(b, tag_slot, int_tag))
      return -1;
  }
  int len = ny_native_nir_emit_const(b, 2);
  if (len < 0 || !ny_native_nir_record_list_len_fact(b, base, len))
    return -1;
  return base;
}
static int ny_native_nir_emit_const_f64(ny_native_nir_builder_t *b, double value) {
  return ny_native_nir_push_val(b, NYIR_CONST_F64, -1, -1,
                                ny_native_f64_bits(value), NULL);
}
static int ny_native_nir_emit_const_f32(ny_native_nir_builder_t *b, double value) {
  return ny_native_nir_push_val(b, NYIR_CONST_F32, -1, -1,
                                ny_native_f32_bits((float)value), NULL);
}
static int ny_native_nir_emit_i64_to_f64(ny_native_nir_builder_t *b, int value) {
  return ny_native_nir_push_val(b, NYIR_I64_TO_F64, value, -1, 0, NULL);
}
static int ny_native_nir_emit_i64_to_f32(ny_native_nir_builder_t *b, int value) {
  return ny_native_nir_push_val(b, NYIR_I64_TO_F32, value, -1, 0, NULL);
}
static int ny_native_nir_emit_f32_to_f64(ny_native_nir_builder_t *b, int value) {
  return ny_native_nir_push_val(b, NYIR_F32_TO_F64, value, -1, 0, NULL);
}
static int ny_native_nir_emit_f64_to_f32(ny_native_nir_builder_t *b, int value) {
  return ny_native_nir_push_val(b, NYIR_F64_TO_F32, value, -1, 0, NULL);
}
static int ny_native_nir_emit_add_i64(ny_native_nir_builder_t *b, int a,
                                      int rhs) {
  return ny_native_nir_push_val(b, NYIR_ADD_I64, a, rhs, 0, NULL);
}
static int ny_native_nir_emit_load_i64(ny_native_nir_builder_t *b, int addr) {
  return ny_native_nir_push_val(b, NYIR_LOAD_I64, addr, -1, 0, NULL);
}
static int ny_native_nir_emit_load8(ny_native_nir_builder_t *b, int addr) {
  int value = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_LOAD_I64,
                                                .dst = -1, .a = addr, .b = -1,
                                                .flags = NYIR_INST_F_MEM_BYTE});
  if (value < 0)
    ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
  return value;
}


static int ny_native_nir_emit_load_f64(ny_native_nir_builder_t *b, int addr) {
  int value = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_LOAD_I64,
                                                .dst = -1, .a = addr, .b = -1,
                                                .flags = NYIR_INST_F_MEM_F64});
  if (value < 0)
    ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
  return value;
}
static int ny_native_nir_emit_addr_local(ny_native_nir_builder_t *b, int slot,
                                         const char *symbol) {
  return ny_native_nir_push_val(b, NYIR_ADDR_LOCAL, -1, -1, slot, symbol);
}
static bool ny_native_nir_emit_store_i64(ny_native_nir_builder_t *b, int addr,
                                         int value) {
  size_t before = b->nyir.len;
  nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_STORE_I64,
                                       .dst = -1,
                                       .a = addr,
                                       .b = -1,
                                       .c = value});
  return b->nyir.len != before ||
         ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
}
static bool ny_native_nir_emit_store8(ny_native_nir_builder_t *b, int addr,
                                       int value) {
  size_t before = b->nyir.len;
  nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_STORE_I64,
                                     .dst = -1, .a = addr, .b = -1, .c = value,
                                     .flags = NYIR_INST_F_MEM_BYTE});
  return b->nyir.len != before ||
         ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
}

static bool ny_native_nir_emit_store_f64(ny_native_nir_builder_t *b, int addr,
                                         int value) {
  size_t before = b->nyir.len;
  nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_STORE_I64,
                                     .dst = -1, .a = addr, .b = -1, .c = value,
                                     .flags = NYIR_INST_F_MEM_F64});
  return b->nyir.len != before || ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
}

/*
 * Emit a bounds check: verifies (base + offset) is within [base, base+byte_len).
 * Elided at lowering time for Fin-typed indices.
 */
static bool ny_native_nir_emit_bounds_check_value(
    ny_native_nir_builder_t *b, int base, int offset, int byte_len_value,
    int64_t byte_len) {
  /*
   * Raw pointers without retained provenance have neither a dynamic nor a
   * static capacity.  A zero static bound is the sentinel for that case.
   */
  if (byte_len_value < 0 && byte_len <= 0)
    return true;
  size_t before = b->nyir.len;
  nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_BOUNDS_CHECK,
                                       .dst = -1,
                                       .a = base,
                                       .b = offset,
                                       .c = byte_len_value,
                                       .imm = byte_len});
  return b->nyir.len != before ||
         ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
}

static bool ny_native_nir_emit_bounds_check(ny_native_nir_builder_t *b,
                                             int base, int offset,
                                             int64_t byte_len) {
  return ny_native_nir_emit_bounds_check_value(b, base, offset, -1, byte_len);
}

static int ny_native_nir_emit_runtime_call(ny_native_nir_builder_t *b,
                                           const char *symbol, int a, int b_arg,
                                           int c, int argc, unsigned flags) {
  int value = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_CALL,
                                                 .dst = -1,
                                                 .a = a,
                                                 .b = b_arg,
                                                 .c = c,
                                                 .imm = argc,
                                                 .flags = NYIR_INST_F_EXTERN | flags,
                                                 .symbol = symbol});
  if (value < 0)
    ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
  return value;
}

/*
 * Forward declaration — defined below with module/block recursion.
 */
static const stmt_t *ny_native_nir_find_user_function(ny_native_nir_builder_t *b,
                                                     const char *name);
static const stmt_t *ny_native_nir_find_extern_decl_in_stmt(
    const stmt_t *s, const char *name, unsigned depth);
bool ny_native_target_eval_bool(const ny_options *opt, const expr_t *e,
                                bool *out);
/*
 * Defined below with the top-level value helpers; used by expr_is_f64 so a
 * member access (gfx.WHITE) or a const list literal types as f64 for the
 * native index/load lowering.
 */
static const expr_t *ny_native_nir_find_top_level_value(
    const ny_native_nir_builder_t *b, const char *name);
static const expr_t *ny_native_nir_resolve_member_expr(
    const ny_native_nir_builder_t *b, const expr_t *e);
static const expr_t *ny_native_nir_resolve_list_literal(
    const ny_native_nir_builder_t *b, const expr_t *e, unsigned depth);

static bool ny_native_nir_expr_is_f64(ny_native_nir_builder_t *b, const expr_t *e) {
  if (!e)
    return false;
  switch (e->kind) {
  case NY_E_LITERAL:
    return e->as.literal.kind == NY_LIT_FLOAT;
  case NY_E_IDENT: {
    ny_native_nir_local_t *l = ny_native_nir_find_local(b, e->as.ident.name);
    if (l)
      return l->is_f64;
    const expr_t *g = ny_native_nir_find_top_level_value(b, e->as.ident.name);
    return g && g != e && g->kind != NY_E_IDENT &&
           ny_native_nir_expr_is_f64(b, g);
  }
  case NY_E_BINARY: {
    nyir_cmp_t ignored;
    if (ny_native_nir_cmp(e->as.binary.op, &ignored))
      return false;
    return ny_native_nir_expr_is_f64(b, e->as.binary.left) ||
           ny_native_nir_expr_is_f64(b, e->as.binary.right);
  }
  case NY_E_UNARY:
    return ny_native_nir_expr_is_f64(b, e->as.unary.right);
  case NY_E_CALL: {
    const char *leaf = ny_native_call_leaf(e);
    ny_native_leaf_kind_t kind = ny_native_leaf_kind(leaf);
    if (kind == NY_NATIVE_LEAF_FLOAT ||
        kind == NY_NATIVE_LEAF_F64BUF_LOAD ||
        kind == NY_NATIVE_LEAF_FLT_SQRT ||
        (leaf && (strcmp(leaf, "__flt_sin") == 0 ||
                  strcmp(leaf, "__flt_cos") == 0)))
      return true;
    if (leaf && (strcmp(leaf, "abs") == 0 || strcmp(leaf, "min") == 0 ||
                 strcmp(leaf, "max") == 0 || strcmp(leaf, "clamp") == 0 ||
                 strcmp(leaf, "lerp") == 0)) {
      for (size_t i = 0; i < e->as.call.args.len; ++i)
        if (ny_native_nir_expr_is_f64(b, e->as.call.args.data[i].val))
          return true;
    }
    if (e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT) {
      const stmt_t *fn = ny_native_nir_find_user_function(
          b, e->as.call.callee->as.ident.name);
      if (fn)
        return ny_native_type_name_is_f64(fn->as.fn.return_type);
    }
    return false;
  }
  case NY_E_MEMCALL:
    if (e->as.memcall.name &&
        (strcmp(e->as.memcall.name, "abs") == 0 ||
         strcmp(e->as.memcall.name, "min") == 0 ||
         strcmp(e->as.memcall.name, "max") == 0 ||
         strcmp(e->as.memcall.name, "clamp") == 0 ||
         strcmp(e->as.memcall.name, "lerp") == 0)) {
      for (size_t i = 0; i < e->as.memcall.args.len; ++i)
        if (ny_native_nir_expr_is_f64(b, e->as.memcall.args.data[i].val))
          return true;
    }
    return false;
  case NY_E_LIST:
    if (e->as.list_like.len == 0)
      return false;
    for (size_t i = 0; i < e->as.list_like.len; ++i)
      if (!ny_native_nir_expr_is_f64(b, e->as.list_like.data[i]))
        return false;
    return true;
  case NY_E_INDEX:
    return ny_native_nir_expr_is_f64(b, e->as.index.target);
  case NY_E_MEMBER: {
    const expr_t *v = ny_native_nir_resolve_member_expr(b, e);
    if (v && v != e)
      return ny_native_nir_expr_is_f64(b, v);
    return false;
  }
  default:
    return false;
  }
}

static bool ny_native_nir_expr_is_f32(ny_native_nir_builder_t *b, const expr_t *e) {
  if (!e)
    return false;
  switch (e->kind) {
  case NY_E_IDENT: {
    ny_native_nir_local_t *l = ny_native_nir_find_local(b, e->as.ident.name);
    if (l)
      return l->is_f32;
    const expr_t *g = ny_native_nir_find_top_level_value(b, e->as.ident.name);
    return g && g != e && g->kind != NY_E_IDENT &&
           ny_native_nir_expr_is_f32(b, g);
  }
  case NY_E_BINARY: {
    nyir_cmp_t ignored;
    if (ny_native_nir_cmp(e->as.binary.op, &ignored))
      return false;
    return ny_native_nir_expr_is_f32(b, e->as.binary.left) ||
           ny_native_nir_expr_is_f32(b, e->as.binary.right);
  }
  case NY_E_UNARY:
    return ny_native_nir_expr_is_f32(b, e->as.unary.right);
  case NY_E_CALL:
    if (e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT) {
      const stmt_t *fn = ny_native_nir_find_user_function(
          b, e->as.call.callee->as.ident.name);
      if (fn)
        return ny_native_type_name_is_f32(fn->as.fn.return_type);
    }
    return false;
  default:
    return false;
  }
}

static bool ny_native_nir_expr_is_cstr(ny_native_nir_builder_t *b,
                                       const expr_t *e) {
  if (!e)
    return false;
  switch (e->kind) {
  case NY_E_LITERAL:
    return e->as.literal.kind == NY_LIT_STR;
  case NY_E_FSTRING:
    return true; /* lowered to a runtime C string */
  case NY_E_IDENT: {
    ny_native_nir_local_t *l = ny_native_nir_find_local(b, e->as.ident.name);
    if (l)
      return l->is_cstr;
    /*
     * Module-level global: classify by its initializer.  Do not chase
     * ident->ident global chains — that keeps this bounded and cycle-free.
     */
    const expr_t *g = ny_native_nir_find_top_level_value(b, e->as.ident.name);
    if (g && g != e && g->kind != NY_E_IDENT)
      return ny_native_nir_expr_is_cstr(b, g);
    return false;
  }
  case NY_E_BINARY:
    return e->as.binary.op && strcmp(e->as.binary.op, "+") == 0 &&
           (ny_native_nir_expr_is_cstr(b, e->as.binary.left) ||
            ny_native_nir_expr_is_cstr(b, e->as.binary.right));
  case NY_E_TERNARY:
    /*
     * A ternary yields a string only when both arms do.
     */
    return ny_native_nir_expr_is_cstr(b, e->as.ternary.true_expr) &&
           ny_native_nir_expr_is_cstr(b, e->as.ternary.false_expr);
  case NY_E_CALL: {
    /*
     * Calls through module aliases carry a member callee.  Resolve both
     * direct and member calls through the same leaf-name path used by native
     * call lowering, then classify from the function's declared return.
     */
    const char *leaf = ny_native_call_leaf(e);
    const stmt_t *fn = leaf ? ny_native_nir_find_user_function(b, leaf) : NULL;
    return fn && fn->as.fn.return_type &&
           strcmp(fn->as.fn.return_type, "str") == 0;
  }
  case NY_E_MEMCALL: {
    const stmt_t *fn = e->as.memcall.name
                           ? ny_native_nir_find_user_function(b, e->as.memcall.name)
                           : NULL;
    return fn && fn->as.fn.return_type &&
           strcmp(fn->as.fn.return_type, "str") == 0;
  }
  default:
    return false;
  }
}

/*
 * True when the expression produces a boolean value (a comparison, logical
 * operation, boolean literal, or boolean unary).  These lower to a raw 0/1
 * i64 in NYIR, so string formatting must route them through
 * rt_native_bool_to_cstr rather than the i64 formatter, which would print
 * the raw 0/1 bits.
 */
static bool ny_native_nir_expr_is_bool(ny_native_nir_builder_t *b,
                                       const expr_t *e) {
  if (!e)
    return false;
  switch (e->kind) {
  case NY_E_LITERAL:
    return e->as.literal.kind == NY_LIT_BOOL;
  case NY_E_BINARY:
    return ny_native_nir_cmp(e->as.binary.op, &(nyir_cmp_t){0});
  case NY_E_LOGICAL:
    return true;
  case NY_E_UNARY:
    return e->as.unary.op && strcmp(e->as.unary.op, "!") == 0;
  case NY_E_TERNARY:
    return ny_native_nir_expr_is_bool(b, e->as.ternary.true_expr) &&
           ny_native_nir_expr_is_bool(b, e->as.ternary.false_expr);
  case NY_E_IDENT: {
    /*
     * A binding initialized from a boolean expression keeps the raw 0/1
     * value, so follow its initializer for classification.  Do not chase
     * ident->ident chains (bounded, cycle-free).
     */
    ny_native_nir_local_t *l = ny_native_nir_find_local(b, e->as.ident.name);
    if (l)
      return l->is_bool;
    const expr_t *g = ny_native_nir_find_top_level_value(b, e->as.ident.name);
    if (g && g != e && g->kind != NY_E_IDENT)
      return ny_native_nir_expr_is_bool(b, g);
    return false;
  }
  default:
    return false;
  }
}

/*
 * True when the expression is any-typed: the value is tagged and requires
 * runtime type dispatch, so it cannot be treated as a raw i64 or pointer.
 */
static bool ny_native_nir_expr_is_any(ny_native_nir_builder_t *b,
                                      const expr_t *e) {
  if (!e)
    return false;
  switch (e->kind) {
  case NY_E_IDENT: {
    ny_native_nir_local_t *l = ny_native_nir_find_local(b, e->as.ident.name);
    if (l)
      return l->is_any;
    const expr_t *g = ny_native_nir_find_top_level_value(b, e->as.ident.name);
    if (g && g != e) {
      if (ny_expr_is_nil_literal(g))
        return true;
      return ny_native_nir_expr_is_any(b, g);
    }
    /*
     * Imported stdlib module state is compiled from its function body, while
     * the defining module variable may not be present in the caller's
     * flattened program tree. Private state names are the only unresolved
     * identifiers admitted here, and only the dynamic-container method path
     * consumes this classification.
     */
    return e->as.ident.name && e->as.ident.name[0] == '_';
  }
  case NY_E_BINARY:
    return e->as.binary.op &&
           (ny_native_nir_expr_is_any(b, e->as.binary.left) ||
            ny_native_nir_expr_is_any(b, e->as.binary.right));
  case NY_E_UNARY:
    return ny_native_nir_expr_is_any(b, e->as.unary.right);
  case NY_E_MEMBER: {
    const expr_t *v = ny_native_nir_resolve_member_expr(b, e);
    return v && v != e && ny_native_nir_expr_is_any(b, v);
  }
  case NY_E_CALL:
    /*
     * The native load64 intrinsics return raw machine i64 values. The
     * language-level `any` return annotations describe the tagged VM path,
     * not the shared NYIR representation.
     */
    if (ny_native_call_leaf(e) &&
        (ny_native_leaf_kind(ny_native_call_leaf(e)) == NY_NATIVE_LEAF_LOAD8 ||
         ny_native_leaf_kind(ny_native_call_leaf(e)) == NY_NATIVE_LEAF_STORE8 ||
         ny_native_leaf_kind(ny_native_call_leaf(e)) == NY_NATIVE_LEAF_LOAD64 ||
         ny_native_leaf_kind(ny_native_call_leaf(e)) == NY_NATIVE_LEAF_LOAD64_IDX ||
         ny_native_leaf_kind(ny_native_call_leaf(e)) == NY_NATIVE_LEAF_STORE64 ||
         ny_native_leaf_kind(ny_native_call_leaf(e)) == NY_NATIVE_LEAF_STORE64_H ||
         ny_native_leaf_kind(ny_native_call_leaf(e)) == NY_NATIVE_LEAF_STORE64_IDX))
      return false;
    if (e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT &&
        e->as.call.callee->as.ident.name) {
      const stmt_t *fn = ny_native_nir_find_user_function(
          b, e->as.call.callee->as.ident.name);
      if (fn)
        return fn->as.fn.return_type &&
               strcmp(fn->as.fn.return_type, "any") == 0;
    }
    break;
  default:
    break;
  }
  return false;
}
static const stmt_t *ny_native_nir_find_user_function_in_stmt(
    const stmt_t *s, const char *name, unsigned depth);
static const expr_t *ny_native_nir_find_top_level_value_in_stmt(
    const stmt_t *s, const char *name, unsigned depth);

/*
 * Compare a known function name against the lookup name, checking both
 * the full path and the leaf (last-dot) component so qualified calls
 * resolve stdlib functions nested inside modules.
 */
static bool ny_native_name_matches(const char *fn_name, const char *lookup) {
  if (!fn_name || !lookup)
    return false;
  if (strcmp(fn_name, lookup) == 0)
    return true;
  const char *fn_leaf = ny_native_leaf_name(fn_name);
  if (fn_leaf && fn_leaf != fn_name && strcmp(fn_leaf, lookup) == 0)
    return true;
  size_t fn_len = strlen(fn_name);
  size_t lookup_len = strlen(lookup);
  return fn_len > lookup_len && fn_name[fn_len - lookup_len - 1] == '.' &&
         strcmp(fn_name + fn_len - lookup_len, lookup) == 0;
}

typedef struct {
  const program_t *prog;
  const stmt_t **funcs;
  size_t count;
  size_t cap;
} ny_native_fn_cache_t;

static ny_native_fn_cache_t ny_native_fn_cache;

static const stmt_t *ny_native_fn_cache_lookup(const char *name) {
  if (!name)
    return NULL;
  const stmt_t *leaf_match = NULL;
  for (size_t i = 0; i < ny_native_fn_cache.count; ++i) {
    const stmt_t *fn = ny_native_fn_cache.funcs[i];
    if (!fn || !fn->as.fn.name)
      continue;
    if (strcmp(fn->as.fn.name, name) == 0)
      return fn;
    if (!leaf_match && ny_native_name_matches(fn->as.fn.name, name))
      leaf_match = fn;
  }
  return leaf_match;
}

static void ny_native_fn_cache_add(const stmt_t *s) {
  if (!s || s->kind != NY_S_FUNC || !s->as.fn.name)
    return;
  if (ny_native_fn_cache.count == ny_native_fn_cache.cap) {
    size_t next = ny_native_fn_cache.cap ? ny_native_fn_cache.cap * 2 : 256;
    const stmt_t **grown = realloc(
        ny_native_fn_cache.funcs, next * sizeof(*ny_native_fn_cache.funcs));
    if (!grown)
      return;
    ny_native_fn_cache.funcs = grown;
    ny_native_fn_cache.cap = next;
  }
  ny_native_fn_cache.funcs[ny_native_fn_cache.count++] = s;
}

static void ny_native_fn_cache_collect(const stmt_t *s, unsigned depth) {
  if (!s || depth > 64)
    return;
  if (s->kind == NY_S_FUNC) {
    ny_native_fn_cache_add(s);
    return;
  }
  if (s->kind == NY_S_MODULE) {
    for (size_t i = 0; i < s->as.module.body.len; ++i)
      ny_native_fn_cache_collect(s->as.module.body.data[i], depth + 1);
    return;
  }
  if (s->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      ny_native_fn_cache_collect(s->as.block.body.data[i], depth + 1);
    return;
  }
  if (s->kind == NY_S_IF) {
    ny_native_fn_cache_collect(s->as.iff.conseq, depth + 1);
    ny_native_fn_cache_collect(s->as.iff.alt, depth + 1);
    return;
  }
  if (s->kind == NY_S_WHILE) {
    ny_native_fn_cache_collect(s->as.whl.body, depth + 1);
    return;
  }
  if (s->kind == NY_S_FOR) {
    ny_native_fn_cache_collect(s->as.fr.body, depth + 1);
    return;
  }
  if (s->kind == NY_S_MATCH) {
    for (size_t i = 0; i < s->as.match.arms.len; ++i)
      ny_native_fn_cache_collect(s->as.match.arms.data[i].conseq, depth + 1);
    ny_native_fn_cache_collect(s->as.match.default_conseq, depth + 1);
    return;
  }
  if (s->kind == NY_S_TRY) {
    ny_native_fn_cache_collect(s->as.tr.body, depth + 1);
    ny_native_fn_cache_collect(s->as.tr.handler, depth + 1);
    return;
  }
  if (s->kind == NY_S_DEFER)
    ny_native_fn_cache_collect(s->as.de.body, depth + 1);
}
static bool ny_native_nir_stmt_has_defer(const stmt_t *s, unsigned depth) {
  if (!s || depth > 64)
    return false;
  switch (s->kind) {
  case NY_S_DEFER:
    return true;
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      if (ny_native_nir_stmt_has_defer(s->as.block.body.data[i], depth + 1))
        return true;
    return false;
  case NY_S_IF:
    return ny_native_nir_stmt_has_defer(s->as.iff.conseq, depth + 1) ||
           ny_native_nir_stmt_has_defer(s->as.iff.alt, depth + 1);
  case NY_S_WHILE:
    return ny_native_nir_stmt_has_defer(s->as.whl.body, depth + 1);
  case NY_S_FOR:
    return ny_native_nir_stmt_has_defer(s->as.fr.body, depth + 1);
  case NY_S_MATCH:
    for (size_t i = 0; i < s->as.match.arms.len; ++i)
      if (ny_native_nir_stmt_has_defer(s->as.match.arms.data[i].conseq,
                                       depth + 1))
        return true;
    return ny_native_nir_stmt_has_defer(s->as.match.default_conseq,
                                        depth + 1);
  case NY_S_TRY:
    return ny_native_nir_stmt_has_defer(s->as.tr.body, depth + 1) ||
           ny_native_nir_stmt_has_defer(s->as.tr.handler, depth + 1);
  default:
    return false;
  }
}

static void ny_native_fn_cache_build(const program_t *prog) {
  if (ny_native_fn_cache.prog == prog)
    return;
  ny_native_fn_cache.prog = prog;
  ny_native_fn_cache.count = 0;
  if (!prog)
    return;
  for (size_t i = 0; i < prog->body.len; ++i)
    ny_native_fn_cache_collect(prog->body.data[i], 0);
}

static const stmt_t *ny_native_nir_find_user_function(ny_native_nir_builder_t *b,
                                                     const char *name) {
  if (!b || !b->prog || !name)
    return NULL;
  if (ny_native_fn_cache.prog == b->prog) {
    const stmt_t *found = ny_native_fn_cache_lookup(name);
    if (found)
      return found;
  }
  for (size_t i = 0; i < b->prog->body.len; ++i) {
    const stmt_t *found = ny_native_nir_find_user_function_in_stmt(
        b->prog->body.data[i], name, 0);
    if (found)
      return found;
  }
  return NULL;
}

/*
 * Recurse into NY_S_MODULE/NY_S_BLOCK/control-flow bodies to find
 * nested function declarations (e.g. stdlib functions inside modules).
 * depth caps at 64 for safety, matching the rest of the codebase.
 */
static const stmt_t *ny_native_nir_find_user_function_in_stmt(
    const stmt_t *s, const char *name, unsigned depth) {
  if (!s || !name || depth > 64)
    return NULL;
  if (s->kind == NY_S_FUNC && s->as.fn.name &&
      ny_native_name_matches(s->as.fn.name, name))
    return s;
  if (s->kind == NY_S_MODULE) {
    for (size_t i = 0; i < s->as.module.body.len; ++i) {
      const stmt_t *found = ny_native_nir_find_user_function_in_stmt(
          s->as.module.body.data[i], name, depth + 1);
      if (found) return found;
    }
    return NULL;
  }
  if (s->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < s->as.block.body.len; ++i) {
      const stmt_t *found = ny_native_nir_find_user_function_in_stmt(
          s->as.block.body.data[i], name, depth + 1);
      if (found) return found;
    }
    return NULL;
  }
  if (s->kind == NY_S_IF) {
    if (s->as.iff.conseq) {
      const stmt_t *found = ny_native_nir_find_user_function_in_stmt(
          s->as.iff.conseq, name, depth + 1);
      if (found) return found;
    }
    if (s->as.iff.alt) {
      const stmt_t *found = ny_native_nir_find_user_function_in_stmt(
          s->as.iff.alt, name, depth + 1);
      if (found) return found;
    }
    return NULL;
  }
  if (s->kind == NY_S_WHILE && s->as.whl.body)
    return ny_native_nir_find_user_function_in_stmt(s->as.whl.body, name, depth + 1);
  if (s->kind == NY_S_FOR && s->as.fr.body)
    return ny_native_nir_find_user_function_in_stmt(s->as.fr.body, name, depth + 1);
  if (s->kind == NY_S_MATCH) {
    for (size_t i = 0; i < s->as.match.arms.len; ++i) {
      const stmt_t *found = ny_native_nir_find_user_function_in_stmt(
          s->as.match.arms.data[i].conseq, name, depth + 1);
      if (found) return found;
    }
    if (s->as.match.default_conseq)
      return ny_native_nir_find_user_function_in_stmt(
          s->as.match.default_conseq, name, depth + 1);
    return NULL;
  }
  return NULL;
}
static const stmt_t *ny_native_nir_find_extern_decl_in_stmt(
    const stmt_t *s, const char *name, unsigned depth) {
  if (!s || !name || depth > 64)
    return NULL;
  if (s->kind == NY_S_EXTERN && s->as.ext.name &&
      ny_native_name_matches(s->as.ext.name, name))
    return s;
  if (s->kind == NY_S_MODULE || s->kind == NY_S_BLOCK) {
    const ny_stmt_list *body =
        s->kind == NY_S_MODULE ? &s->as.module.body : &s->as.block.body;
    for (size_t i = 0; i < body->len; ++i) {
      const stmt_t *found = ny_native_nir_find_extern_decl_in_stmt(
          body->data[i], name, depth + 1);
      if (found)
        return found;
    }
    return NULL;
  }
  if (s->kind == NY_S_IF) {
    const stmt_t *found = ny_native_nir_find_extern_decl_in_stmt(
        s->as.iff.conseq, name, depth + 1);
    return found ? found : ny_native_nir_find_extern_decl_in_stmt(
                               s->as.iff.alt, name, depth + 1);
  }
  if (s->kind == NY_S_WHILE) {
    const stmt_t *found = ny_native_nir_find_extern_decl_in_stmt(
        s->as.whl.init, name, depth + 1);
    if (!found)
      found = ny_native_nir_find_extern_decl_in_stmt(s->as.whl.body, name,
                                                     depth + 1);
    return found ? found : ny_native_nir_find_extern_decl_in_stmt(
                               s->as.whl.update, name, depth + 1);
  }
  if (s->kind == NY_S_FOR) {
    const stmt_t *found = ny_native_nir_find_extern_decl_in_stmt(
        s->as.fr.init, name, depth + 1);
    if (!found)
      found = ny_native_nir_find_extern_decl_in_stmt(s->as.fr.body, name,
                                                     depth + 1);
    return found ? found : ny_native_nir_find_extern_decl_in_stmt(
                               s->as.fr.update, name, depth + 1);
  }
  if (s->kind == NY_S_TRY) {
    const stmt_t *found = ny_native_nir_find_extern_decl_in_stmt(
        s->as.tr.body, name, depth + 1);
    return found ? found : ny_native_nir_find_extern_decl_in_stmt(
                               s->as.tr.handler, name, depth + 1);
  }
  if (s->kind == NY_S_DEFER)
    return ny_native_nir_find_extern_decl_in_stmt(s->as.de.body, name,
                                                  depth + 1);
  if (s->kind == NY_S_MATCH) {
    for (size_t i = 0; i < s->as.match.arms.len; ++i) {
      const stmt_t *found = ny_native_nir_find_extern_decl_in_stmt(
          s->as.match.arms.data[i].conseq, name, depth + 1);
      if (found)
        return found;
    }
    return ny_native_nir_find_extern_decl_in_stmt(s->as.match.default_conseq,
                                                  name, depth + 1);
  }
  return NULL;
}

/*
 * Native-only lowering compiles bodies for all non-extern Nytrix functions
 * found anywhere in the program (top-level or nested inside modules/blocks).
 * A function that is neither extern nor has a body found anywhere cannot be
 * called — reject it so the caller gets a clean error instead of an
 * undefined ny_fn_* symbol at link time.
 */
static bool ny_native_nir_fn_has_compiled_body(ny_native_nir_builder_t *b,
                                               const char *name) {
  if (!b || !b->prog || !name)
    return false;
  /*
   * Search the entire program tree (root + modules/blocks) for any
   * non-extern function whose name matches.
   */
  const stmt_t *fn = ny_native_nir_find_user_function(b, name);
  if (!fn)
    return false;
  /*
   * extern/link_name functions are resolved by symbol, not by compiled body.
   */
  if (fn->as.fn.is_extern || fn->as.fn.link_name)
    return false;
  return true;
}

/*
 * Legacy alias used by callers that pre-date the recursive search.
 */
static bool ny_native_nir_user_fn_is_top_level(ny_native_nir_builder_t *b,
                                               const char *name) {
  return ny_native_nir_fn_has_compiled_body(b, name);
}

/*
 * True when a user (non-stdlib) function named `name` is defined anywhere
 * in the program.  Native leaf names (print, assert, argc, ...) are
 * lowered directly to runtime calls unless a user function shadows the
 * leaf name.  Stdlib definitions must not count: ny_native_add_reachable_fn
 * deliberately skips leaf-named stdlib bodies (they use NY_E_LIST /
 * NY_E_MEMBER constructs the shared lowerer does not support), so the leaf
 * path has to fire for stdlib `print`/`assert`/`argc` or the generic call
 * path would emit a reference to a body that is never compiled.
 */
static bool ny_native_nir_user_defined_fn(ny_native_nir_builder_t *b,
                                          const char *name) {
  if (!b || !b->prog || !name)
    return false;
  const stmt_t *fn = ny_native_nir_find_user_function(b, name);
  if (!fn || fn->as.fn.is_extern || fn->as.fn.link_name)
    return false;
  return !ny_is_stdlib_tok(fn->tok);
}

static const expr_t *ny_native_nir_find_top_level_value(
    const ny_native_nir_builder_t *b, const char *name) {
  if (!b || !b->prog || !name)
    return NULL;
  for (size_t i = 0; i < b->prog->body.len; ++i) {
    const expr_t *found = ny_native_nir_find_top_level_value_in_stmt(
        b->prog->body.data[i], name, 0);
    if (found)
      return found;
  }
  return NULL;
}

/*
 * Recurse into container statements to find top-level variable values
 * nested inside modules/blocks (e.g. stdlib constants).
 */
static const expr_t *ny_native_nir_find_top_level_value_in_stmt(
    const stmt_t *s, const char *name, unsigned depth) {
  if (!s || !name || depth > 64)
    return NULL;
  if (s->kind == NY_S_VAR) {
    for (size_t n = 0; n < s->as.var.names.len && n < s->as.var.exprs.len; ++n)
      if (s->as.var.names.data[n] &&
          strcmp(s->as.var.names.data[n], name) == 0)
        return s->as.var.exprs.data[n];
    return NULL;
  }
  if (s->kind == NY_S_MODULE) {
    for (size_t i = 0; i < s->as.module.body.len; ++i) {
      const expr_t *found = ny_native_nir_find_top_level_value_in_stmt(
          s->as.module.body.data[i], name, depth + 1);
      if (found) return found;
    }
    return NULL;
  }
  if (s->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < s->as.block.body.len; ++i) {
      const expr_t *found = ny_native_nir_find_top_level_value_in_stmt(
          s->as.block.body.data[i], name, depth + 1);
      if (found) return found;
    }
    return NULL;
  }
  return NULL;
}

/*
 * Resolve a `use module as alias` declaration (recursively, so aliases
 * declared inside modules resolve too) to the module path.  Returns NULL
 * when no use statement declares the alias.
 */
static const char *ny_native_nir_use_alias_in_stmt(const stmt_t *s,
                                                   const char *alias,
                                                   unsigned depth) {
  if (!s || !alias || depth > 64)
    return NULL;
  if (s->kind == NY_S_USE && s->as.use.alias &&
      strcmp(s->as.use.alias, alias) == 0)
    return s->as.use.module;
  if (s->kind == NY_S_MODULE) {
    for (size_t i = 0; i < s->as.module.body.len; ++i) {
      const char *found = ny_native_nir_use_alias_in_stmt(
          s->as.module.body.data[i], alias, depth + 1);
      if (found)
        return found;
    }
  }
  if (s->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < s->as.block.body.len; ++i) {
      const char *found = ny_native_nir_use_alias_in_stmt(
          s->as.block.body.data[i], alias, depth + 1);
      if (found)
        return found;
    }
  }
  return NULL;
}

static const char *ny_native_nir_resolve_use_alias(
    const ny_native_nir_builder_t *b, const char *alias) {
  if (!b || !b->prog || !alias)
    return NULL;
  for (size_t i = 0; i < b->prog->body.len; ++i) {
    const char *found = ny_native_nir_use_alias_in_stmt(b->prog->body.data[i],
                                                        alias, 0);
    if (found)
      return found;
  }
  return NULL;
}

static const stmt_t *ny_native_nir_find_module(
    const stmt_t *s, const char *name, unsigned depth) {
  if (!s || !name || depth > 64)
    return NULL;
  if (s->kind == NY_S_MODULE) {
    if (s->as.module.name && ny_native_name_matches(s->as.module.name, name))
      return s;
    for (size_t i = 0; i < s->as.module.body.len; ++i) {
      const stmt_t *found = ny_native_nir_find_module(
          s->as.module.body.data[i], name, depth + 1);
      if (found)
        return found;
    }
  } else if (s->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < s->as.block.body.len; ++i) {
      const stmt_t *found = ny_native_nir_find_module(
          s->as.block.body.data[i], name, depth + 1);
      if (found)
        return found;
    }
  }
  return NULL;
}

/*
 * Find a NY_S_VAR statement named `member` inside a module body.
 */
static const expr_t *ny_native_nir_find_module_member(
    const stmt_t *mod, const char *member, unsigned depth) {
  if (!mod || !member || depth > 64)
    return NULL;
  if (mod->kind == NY_S_VAR) {
    for (size_t n = 0; n < mod->as.var.names.len && n < mod->as.var.exprs.len;
         ++n)
      if (mod->as.var.names.data[n] &&
          strcmp(mod->as.var.names.data[n], member) == 0)
        return mod->as.var.exprs.data[n];
    return NULL;
  }
  if (mod->kind == NY_S_MODULE) {
    for (size_t i = 0; i < mod->as.module.body.len; ++i) {
      const expr_t *found = ny_native_nir_find_module_member(
          mod->as.module.body.data[i], member, depth + 1);
      if (found)
        return found;
    }
    return NULL;
  }
  if (mod->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < mod->as.block.body.len; ++i) {
      const expr_t *found = ny_native_nir_find_module_member(
          mod->as.block.body.data[i], member, depth + 1);
      if (found)
        return found;
    }
    return NULL;
  }
  return NULL;
}

/*
 * Resolve a constant re-exported through a module's `use` declarations.
 * Constant defs are stored as flat top-level values under their defining
 * module's fully-qualified name (e.g. "std.os.ui.window.platform.api.CLIENT_API"),
 * so a re-exporting module owns no child def for the name.  This walks the
 * module's `use` re-exports, tries "<imported>.<member>" as a top-level value,
 * and recurses into the imported module's own re-exports.  `visited` bounds the
 * search over diamond/cyclic imports so it can never blow up.
 */
static const expr_t *ny_native_nir_find_imported_module_member_rec(
    const ny_native_nir_builder_t *b, const stmt_t *mod, const char *member,
    unsigned depth, const stmt_t **visited, size_t *visited_len,
    size_t visited_cap) {
  if (!b || !b->prog || !mod || !member || depth > 64)
    return NULL;
  for (size_t v = 0; v < *visited_len; ++v)
    if (visited[v] == mod)
      return NULL;
  if (*visited_len < visited_cap)
    visited[(*visited_len)++] = mod;
  else
    return NULL; /* Explicit bound: refuse to search past the visited cap. */
  const ny_stmt_list *body = NULL;
  if (mod->kind == NY_S_MODULE)
    body = &mod->as.module.body;
  else if (mod->kind == NY_S_BLOCK)
    body = &mod->as.block.body;
  if (!body)
    return NULL;
  for (size_t i = 0; i < body->len; ++i) {
    const stmt_t *use = body->data[i];
    if (!use || use->kind != NY_S_USE || !use->as.use.module)
      continue;
    bool imports_member = use->as.use.import_all || use->as.use.imports.len == 0;
    for (size_t n = 0; !imports_member && n < use->as.use.imports.len; ++n) {
      const use_item_t *item = &use->as.use.imports.data[n];
      const char *visible = item->alias ? item->alias : item->name;
      if (visible && strcmp(visible, member) == 0)
        imports_member = true;
    }
    if (!imports_member)
      continue;
    const char *imported_name = use->as.use.module;
    /*
     * Direct: the imported module defines the constant itself.
     */
    char qualified[1024];
    int qn = snprintf(qualified, sizeof(qualified), "%s.%s", imported_name,
                      member);
    if (qn > 0 && (size_t)qn < sizeof(qualified)) {
      const expr_t *v = ny_native_nir_find_top_level_value(b, qualified);
      if (v)
        return v;
    }
    /*
     * Transitive: the imported module itself re-exports the constant.
     */
    for (size_t r = 0; r < b->prog->body.len; ++r) {
      const stmt_t *imported = ny_native_nir_find_module(
          b->prog->body.data[r], imported_name, 0);
      if (!imported || imported == mod)
        continue;
      const expr_t *value = ny_native_nir_find_imported_module_member_rec(
          b, imported, member, depth + 1, visited, visited_len, visited_cap);
      if (value)
        return value;
    }
  }
  return NULL;
}

/*
 * Resolve constants re-exported through a module's use declarations.
 */
static const expr_t *ny_native_nir_find_imported_module_member(
    const ny_native_nir_builder_t *b, const stmt_t *mod, const char *member,
    unsigned depth) {
  const stmt_t *visited[256];
  size_t visited_len = 0;
  return ny_native_nir_find_imported_module_member_rec(
      b, mod, member, depth, visited, &visited_len, 256);
}

/*
 * Resolve a member access (gfx.WHITE) to the referenced top-level def's
 * initializer expr: alias/module path first, then a plain-name global
 * search.  Returns NULL when the member is not a constant/module value.
 */
static const expr_t *ny_native_nir_resolve_member_expr(
    const ny_native_nir_builder_t *b, const expr_t *e) {
  if (!b || !e || e->kind != NY_E_MEMBER || !e->as.member.target ||
      !e->as.member.name)
    return NULL;

  /*
   * Flatten a (possibly nested) dotted member chain such as
   * `std.math.big._TAG_LIST` into a root identifier plus its path
   * components.  The chain is left-nested, so walking from the leaf down
   * collects [leaf, ..., component-nearest-root] and ends at the root IDENT.
   */
  const char *path[16];
  size_t npath = 0;
  const expr_t *cur = e;
  while (cur && cur->kind == NY_E_MEMBER && npath < 16) {
    path[npath++] = cur->as.member.name;
    cur = cur->as.member.target;
  }
  if (!cur || cur->kind != NY_E_IDENT || !cur->as.ident.name || npath == 0)
    return NULL;
  const char *root = cur->as.ident.name;

  /*
   * Build the module path: the root resolved through its use alias, then the
   * intermediate components in reverse order.  path[0] is the member name.
   */
  char module[512];
  const char *resolved = ny_native_nir_resolve_use_alias(b, root);
  size_t mlen = (size_t)snprintf(module, sizeof(module), "%s",
                                 resolved ? resolved : root);
  if (mlen >= sizeof(module))
    return NULL;
  for (size_t i = npath; i > 1; --i) {
    int n = snprintf(module + mlen, sizeof(module) - mlen, ".%s",
                     path[i - 1]);
    if (n < 0 || (size_t)n >= sizeof(module) - mlen)
      return NULL;
    mlen += (size_t)n;
  }

  for (size_t i = 0; i < b->prog->body.len; ++i) {
    const stmt_t *mod =
        ny_native_nir_find_module(b->prog->body.data[i], module, 0);
    if (mod) {
      const expr_t *v = ny_native_nir_find_module_member(mod, path[0], 0);
      if (!v)
        v = ny_native_nir_find_imported_module_member(b, mod, path[0], 0);
      if (v)
        return v;
    }
  }
  char qualified[1024];
  int n = snprintf(qualified, sizeof(qualified), "%s.%s", module, path[0]);
  if (n > 0 && (size_t)n < sizeof(qualified)) {
    const expr_t *v = ny_native_nir_find_top_level_value(b, qualified);
    if (v)
      return v;
  }
  return ny_native_nir_find_top_level_value(b, path[0]);
}

/*
 * Resolve an expression to a constant list literal when it is one directly,
 * via a top-level def, or via a member access.  Used for .len folding and
 * index element-type detection.
 */
static bool ny_native_nir_expr_is_list(
    const ny_native_nir_builder_t *b, const expr_t *e) {
  if (!b || !e)
    return false;
  if (e->kind == NY_E_LIST)
    return true;
  if (e->kind == NY_E_BINARY && e->as.binary.op &&
      strcmp(e->as.binary.op, "*") == 0) {
    bool left_list = ny_native_nir_expr_is_list(b, e->as.binary.left);
    bool right_list = ny_native_nir_expr_is_list(b, e->as.binary.right);
    return left_list != right_list;
  }
  if (e->kind == NY_E_CALL) {
    const char *leaf = ny_native_call_leaf(e);
    const stmt_t *fn = leaf ? ny_native_nir_find_user_function(
                                  (ny_native_nir_builder_t *)b, leaf)
                            : NULL;
    return fn && ny_native_type_name_is_list(fn->as.fn.return_type);
  }
  if (e->kind == NY_E_MEMCALL) {
    const stmt_t *fn = e->as.memcall.name
                           ? ny_native_nir_find_user_function(
                                 (ny_native_nir_builder_t *)b,
                                 e->as.memcall.name)
                           : NULL;
    return fn && ny_native_type_name_is_list(fn->as.fn.return_type);
  }
  if (e->kind == NY_E_IDENT) {
    ny_native_nir_local_t *local = ny_native_nir_find_local(
        (ny_native_nir_builder_t *)b, e->as.ident.name);
    if (local)
      return local->is_list;
    const expr_t *global =
        ny_native_nir_find_top_level_value(b, e->as.ident.name);
    if (global && global != e && global->kind != NY_E_IDENT)
      return ny_native_nir_expr_is_list(b, global);
    return false;
  }
  return false;
}

static const expr_t *ny_native_nir_resolve_list_literal(
    const ny_native_nir_builder_t *b, const expr_t *e, unsigned depth) {
  if (!b || !e || depth > 64)
    return NULL;
  if (e->kind == NY_E_LIST)
    return e;
  if (e->kind == NY_E_IDENT) {
    ny_native_nir_local_t *local =
        ny_native_nir_find_local((ny_native_nir_builder_t *)b,
                                 e->as.ident.name);
    if (local && local->list_literal)
      return local->list_literal;
    const expr_t *v = ny_native_nir_find_top_level_value(b, e->as.ident.name);
    if (v && v != e)
      return ny_native_nir_resolve_list_literal(b, v, depth + 1);
    return NULL;
  }
  if (e->kind == NY_E_MEMBER) {
    const expr_t *v = ny_native_nir_resolve_member_expr(b, e);
    if (v && v != e)
      return ny_native_nir_resolve_list_literal(b, v, depth + 1);
    return NULL;
  }
  return NULL;
}

/*
 * Fold a top-level def initializer to a raw i64 when it is a compile-time
 * constant (literals, references to other top-level defs, integer unary and
 * binary ops).  Used to register the def's value in the consttab so object
 * emission writes an 8-byte .data definition for the symbol.
 */
static bool ny_native_nir_fold_top_level_int(const program_t *prog,
                                             const expr_t *e, int64_t *out,
                                             unsigned depth) {
  if (!e || !out || depth > 64)
    return false;
  switch (e->kind) {
  case NY_E_LITERAL:
    if (e->as.literal.kind == NY_LIT_BOOL) {
      *out = e->as.literal.as.b ? 1 : 0;
      return true;
    }
    if (e->as.literal.kind == NY_LIT_INT && e->tok.kind != NY_T_NIL) {
      *out = e->as.literal.as.i;
      return true;
    }
    return false;
  case NY_E_IDENT: {
    if (!prog || !e->as.ident.name)
      return false;
    for (size_t i = 0; i < prog->body.len; ++i) {
      const expr_t *val = ny_native_nir_find_top_level_value_in_stmt(
          prog->body.data[i], e->as.ident.name, 0);
      if (val && val != e)
        return ny_native_nir_fold_top_level_int(prog, val, out, depth + 1);
    }
    return false;
  }
  case NY_E_UNARY: {
    if (!e->as.unary.op || !e->as.unary.right)
      return false;
    int64_t v = 0;
    if (!ny_native_nir_fold_top_level_int(prog, e->as.unary.right, &v,
                                          depth + 1))
      return false;
    if (strcmp(e->as.unary.op, "+") == 0) {
      *out = v;
      return true;
    }
    if (strcmp(e->as.unary.op, "-") == 0) {
      *out = -v;
      return true;
    }
    if (strcmp(e->as.unary.op, "!") == 0) {
      *out = v ? 0 : 1;
      return true;
    }
    return false;
  }
  case NY_E_BINARY: {
    if (!e->as.binary.op || !e->as.binary.left || !e->as.binary.right)
      return false;
    if (strcmp(e->as.binary.op, "^") == 0)
      return ny_native_nir_fold_const_pow(e->as.binary.left,
                                          e->as.binary.right, out);
    int64_t l = 0, r = 0;
    if (!ny_native_nir_fold_top_level_int(prog, e->as.binary.left, &l,
                                          depth + 1) ||
        !ny_native_nir_fold_top_level_int(prog, e->as.binary.right, &r,
                                          depth + 1))
      return false;
    nyir_op_t op;
    if (!ny_native_nir_binop(e->as.binary.op, &op))
      return false;
    switch (op) {
    case NYIR_ADD_I64: *out = l + r; return true;
    case NYIR_SUB_I64: *out = l - r; return true;
    case NYIR_MUL_I64: *out = l * r; return true;
    case NYIR_DIV_I64: if (r == 0) return false; *out = l / r; return true;
    case NYIR_MOD_I64: if (r == 0) return false; *out = l % r; return true;
    case NYIR_AND_I64: *out = l & r; return true;
    case NYIR_OR_I64:  *out = l | r; return true;
    case NYIR_XOR_I64: *out = l ^ r; return true;
    case NYIR_SHL_I64: if (r < 0 || r >= 64) return false; *out = l << r; return true;
    case NYIR_SAR_I64: if (r < 0 || r >= 64) return false; *out = l >> r; return true;
    default: return false;
    }
  }
  default:
    return false;
  }
}

/*
 * Fold a call to __runtime_tag("name") / runtime_tag_raw("name") with a
 * string-literal argument to its tag constant.  Shared by the call-site
 * lowering and top-level consttab registration.
 */
static bool ny_native_nir_fold_runtime_tag(const expr_t *call, int64_t *out) {
  if (!call || !out || call->kind != NY_E_CALL)
    return false;
  const char *leaf = ny_native_call_leaf(call);
  if (!leaf || (strcmp(leaf, "__runtime_tag") != 0 &&
                strcmp(leaf, "runtime_tag_raw") != 0))
    return false;
  if (call->as.call.args.len != 1 || call->as.call.args.data[0].name ||
      !call->as.call.args.data[0].val ||
      call->as.call.args.data[0].val->kind != NY_E_LITERAL ||
      call->as.call.args.data[0].val->as.literal.kind != NY_LIT_STR)
    return false;
  const char *s = call->as.call.args.data[0].val->as.literal.as.s.data;
  size_t n = call->as.call.args.data[0].val->as.literal.as.s.len;
  *out = rt_tag_v(rt_runtime_tag_raw_name(s, n));
  return true;
}

/*
 * Register immutable integer/float definitions nested in stdlib modules.
 */
static void ny_native_register_const_defs(const program_t *prog,
                                          const stmt_t *s) {
  if (!prog || !s)
    return;
  if (s->kind == NY_S_DEFINE) {
    if (s->as.def.name && *s->as.def.name && s->as.def.value &&
        *s->as.def.value) {
      char *end = NULL;
      errno = 0;
      long long parsed = strtoll(s->as.def.value, &end, 0);
      while (end && *end && isspace((unsigned char)*end))
        ++end;
      if (end && end != s->as.def.value && *end == '\0' && errno == 0)
        ny_native_consttab_add(s->as.def.name, (int64_t)parsed);
    }
    return;
  }
  if (s->kind == NY_S_VAR) {
    if (s->as.var.is_mut)
      return;
    for (size_t i = 0;
         i < s->as.var.names.len && i < s->as.var.exprs.len; ++i) {
      const char *name = s->as.var.names.data[i];
      const expr_t *init = s->as.var.exprs.data[i];
      if (!name || !*name || !init)
        continue;
      int64_t value = 0;
      bool folded = ny_native_nir_fold_top_level_int(prog, init, &value, 0);
      bool is_tag = false;
      if (!folded && init->kind == NY_E_LITERAL &&
          init->as.literal.kind == NY_LIT_FLOAT) {
        value = ny_native_f64_bits(init->as.literal.as.f);
        folded = true;
      }
      if (!folded && ny_native_nir_fold_runtime_tag(init, &value)) {
        folded = true;
        is_tag = true;
      }
      if (folded) {
        ny_native_consttab_add(name, value);
        /*
         * Stdlib module defs carry their module path as a prefix
         * (std.math.big._TAG_BIGINT).  Bare intra-module references use the
         * leaf name only, so also register the leaf.  Restricted to tag
         * constants: __runtime_tag("x") yields the same value in every
         * module, so a leaf-name collision cannot resolve to the wrong
         * constant (unlike int/float defs, which may differ per module).
         */
        if (is_tag) {
          const char *leaf = strrchr(name, '.');
          if (leaf && leaf[1])
            ny_native_consttab_add(leaf + 1, value);
        }
      }
    }
    return;
  }
  if (s->kind == NY_S_MODULE) {
    for (size_t i = 0; i < s->as.module.body.len; ++i)
      ny_native_register_const_defs(prog, s->as.module.body.data[i]);
    return;
  }
  if (s->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      ny_native_register_const_defs(prog, s->as.block.body.data[i]);
  }
}

static ny_native_nir_local_t *ny_native_nir_find_local_slot(
    ny_native_nir_builder_t *b, int slot) {
  if (!b)
    return NULL;
  for (size_t i = b->local_count; i > 0; --i)
    if (b->locals[i - 1].slot == slot)
      return &b->locals[i - 1];
  return NULL;
}

static bool ny_native_nir_record_fact(ny_native_nir_builder_t *b, int value,
                                      ny_native_nir_fact_kind_t kind,
                                      int64_t payload) {
  if (!b || value < 0 || payload <= 0)
    return true;
  if (b->fact_count == b->fact_cap) {
    size_t cap = b->fact_cap ? b->fact_cap * 2 : 16;
    if (cap < b->fact_cap || cap > SIZE_MAX / sizeof(*b->facts))
      return ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
    ny_native_nir_fact_t *facts =
        realloc(b->facts, cap * sizeof(*facts));
    if (!facts)
      return ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
    b->facts = facts;
    b->fact_cap = cap;
  }
  b->facts[b->fact_count++] =
      (ny_native_nir_fact_t){.value = value, .payload = payload, .kind = kind};
  return true;
}

static int64_t ny_native_nir_peek_fact(const ny_native_nir_builder_t *b,
                                       int value,
                                       ny_native_nir_fact_kind_t kind) {
  if (!b || value < 0)
    return 0;
  for (size_t i = b->fact_count; i > 0; --i) {
    size_t index = i - 1;
    if (b->facts[index].value == value && b->facts[index].kind == kind)
      return b->facts[index].payload;
  }
  return 0;
}

static int64_t ny_native_nir_take_fact(ny_native_nir_builder_t *b, int value,
                                       ny_native_nir_fact_kind_t kind) {
  if (!b || value < 0)
    return 0;
  for (size_t i = b->fact_count; i > 0; --i) {
    size_t index = i - 1;
    if (b->facts[index].value != value || b->facts[index].kind != kind)
      continue;
    int64_t payload = b->facts[index].payload;
    b->facts[index] = b->facts[--b->fact_count];
    return payload;
  }
  return 0;
}

static bool ny_native_nir_record_alloc_fact(ny_native_nir_builder_t *b,
                                            int value, int64_t byte_len) {
  return ny_native_nir_record_fact(b, value, NY_NATIVE_NIR_FACT_ALLOC,
                                    byte_len);
}

static int64_t ny_native_nir_peek_alloc_fact(const ny_native_nir_builder_t *b,
                                             int value) {
  return ny_native_nir_peek_fact(b, value, NY_NATIVE_NIR_FACT_ALLOC);
}

static int64_t ny_native_nir_take_alloc_fact(ny_native_nir_builder_t *b,
                                             int value) {
  return ny_native_nir_take_fact(b, value, NY_NATIVE_NIR_FACT_ALLOC);
}

static bool ny_native_nir_record_fin_fact(ny_native_nir_builder_t *b,
                                          int value, int64_t bound) {
  return ny_native_nir_record_fact(b, value, NY_NATIVE_NIR_FACT_FIN, bound);
}

static int64_t ny_native_nir_take_fin_fact(ny_native_nir_builder_t *b,
                                           int value) {
  return ny_native_nir_take_fact(b, value, NY_NATIVE_NIR_FACT_FIN);
}

static bool ny_native_nir_record_list_len_fact(ny_native_nir_builder_t *b,
                                                int value, int length_value) {
  return length_value < 0 || length_value == INT32_MAX
             ? false
             : ny_native_nir_record_fact(b, value,
                                         NY_NATIVE_NIR_FACT_LIST_LEN,
                                         (int64_t)length_value + 1);
}

static int ny_native_nir_peek_list_len_fact(const ny_native_nir_builder_t *b,
                                             int value) {
  int64_t encoded =
      ny_native_nir_peek_fact(b, value, NY_NATIVE_NIR_FACT_LIST_LEN);
  return encoded > 0 ? (int)(encoded - 1) : -1;
}

static int ny_native_nir_take_list_len_fact(ny_native_nir_builder_t *b,
                                             int value) {
  int64_t encoded =
      ny_native_nir_take_fact(b, value, NY_NATIVE_NIR_FACT_LIST_LEN);
  return encoded > 0 ? (int)(encoded - 1) : -1;
}

static bool ny_native_nir_record_dyn_fact(ny_native_nir_builder_t *b,
                                          int value,
                                          ny_native_nir_fact_kind_t kind,
                                          int reg) {
  return reg >= 0 &&
         ny_native_nir_record_fact(b, value, kind, (int64_t)reg + 1);
}

static int ny_native_nir_peek_dyn_fact(const ny_native_nir_builder_t *b,
                                       int value,
                                       ny_native_nir_fact_kind_t kind) {
  int64_t encoded = ny_native_nir_peek_fact(b, value, kind);
  return encoded > 0 ? (int)(encoded - 1) : -1;
}

static int ny_native_nir_take_dyn_fact(ny_native_nir_builder_t *b, int value,
                                       ny_native_nir_fact_kind_t kind) {
  int64_t encoded = ny_native_nir_take_fact(b, value, kind);
  return encoded > 0 ? (int)(encoded - 1) : -1;
}

static int ny_native_nir_emit_known_list_append_len(
    ny_native_nir_builder_t *b, int list, int out) {
  int length = -1;
  int base_len = ny_native_nir_take_list_len_fact(b, list);
  if (base_len >= 0) {
    int one = ny_native_nir_emit_const(b, 1);
    length = one < 0 ? -1 : ny_native_nir_emit_add_i64(b, base_len, one);
  }
  if (length < 0 && out >= 0)
    length = ny_native_nir_emit_runtime_call(
        b, "rt_native_tbuf_len", out, -1, -1, 1, 0);
  return length;
}

/*
 * A tbuf pop never reallocates; it only decrements the header count when the
 * list is non-empty.  When lowering already carries the list length as an SSA
 * fact, derive the post-pop length directly instead of re-reading the managed
 * header through rt_native_tbuf_len.  CMP_I64 yields 0/1, so
 *   len - (len > 0)
 * exactly preserves pop's saturating-at-zero length semantics.
 */
static int ny_native_nir_emit_known_list_pop_len(
    ny_native_nir_builder_t *b, int list) {
  int base_len = ny_native_nir_take_list_len_fact(b, list);
  if (base_len < 0)
    return -1;
  int zero = ny_native_nir_emit_const(b, 0);
  if (zero < 0)
    return -1;
  int nonempty = nyir_emit(&b->nyir,
                           (nyir_inst_t){.op = NYIR_CMP_I64, .dst = -1,
                                         .a = base_len, .b = zero,
                                         .cmp = NYIR_CMP_GT});
  if (nonempty < 0) {
    ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
    return -1;
  }
  return ny_native_nir_push_val(b, NYIR_SUB_I64, base_len, nonempty, 0, NULL);
}

static int ny_native_nir_emit_known_cstr_concat_len(
    ny_native_nir_builder_t *b, int lhs, int rhs, int out) {
  int lhs_len =
      ny_native_nir_peek_dyn_fact(b, lhs, NY_NATIVE_NIR_FACT_DYN_STR_LEN);
  int rhs_len =
      ny_native_nir_peek_dyn_fact(b, rhs, NY_NATIVE_NIR_FACT_DYN_STR_LEN);
  if (lhs_len >= 0 && rhs_len >= 0)
    return ny_native_nir_emit_add_i64(b, lhs_len, rhs_len);
  return out < 0 ? -1
                 : ny_native_nir_emit_runtime_call(
                       b, "rt_native_cstr_len", out, -1, -1, 1, 0);
}

static int64_t ny_native_nir_literal_allocation_size(const char *leaf,
                                                     const expr_t *call) {
  if (!leaf || !call || call->kind != NY_E_CALL)
    return 0;
  ny_builtin_alloc_kind_t kind = ny_builtin_alloc_kind(leaf);
  if (kind == NY_BUILTIN_ALLOC_NONE || kind == NY_BUILTIN_ALLOC_FREE)
    return 0;
  size_t size_arg = kind == NY_BUILTIN_ALLOC_REALLOC ? 1 : 0;
  if (call->as.call.args.len <= size_arg)
    return 0;
  const expr_t *size = call->as.call.args.data[size_arg].val;
  if (!size || size->kind != NY_E_LITERAL ||
      size->as.literal.kind != NY_LIT_INT || size->as.literal.as.i <= 0)
    return 0;
  int64_t bytes = size->as.literal.as.i;
  if (kind != NY_BUILTIN_ALLOC_CALLOC)
    return bytes;
  if (call->as.call.args.len != 2)
    return 0;
  const expr_t *count = call->as.call.args.data[0].val;
  if (!count || count->kind != NY_E_LITERAL ||
      count->as.literal.kind != NY_LIT_INT || count->as.literal.as.i <= 0 ||
      count->as.literal.as.i > INT64_MAX / bytes)
    return 0;
  return count->as.literal.as.i * bytes;
}

static bool ny_native_nir_store_local_value(ny_native_nir_builder_t *b,
                                            int slot, int value) {
  size_t before = b->nyir.len;
  nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_STORE_LOCAL,
                                       .dst = -1,
                                       .a = value,
                                       .b = -1,
                                       .imm = slot});
  if (b->nyir.len == before)
    return ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
  ny_native_nir_local_t *local = ny_native_nir_find_local_slot(b, slot);
  int64_t byte_len = ny_native_nir_take_alloc_fact(b, value);
  int64_t fin_bound = ny_native_nir_take_fin_fact(b, value);
  int list_len = ny_native_nir_take_list_len_fact(b, value);
  int dyn_str_len = ny_native_nir_take_dyn_fact(b, value, NY_NATIVE_NIR_FACT_DYN_STR_LEN);
  int dyn_tag = ny_native_nir_take_dyn_fact(b, value, NY_NATIVE_NIR_FACT_DYN_TAG);
  if (local) {
    local->buffer_byte_len = byte_len;
    local->fin_bound = fin_bound;
    if (dyn_str_len >= 0 && dyn_tag >= 0) {
      if (local->dyn_str_len_slot < 0) local->dyn_str_len_slot = b->next_local_slot++;
      if (local->dyn_tag_slot < 0) local->dyn_tag_slot = b->next_local_slot++;
      if (!ny_native_nir_store_local_value(b, local->dyn_str_len_slot, dyn_str_len) ||
          !ny_native_nir_store_local_value(b, local->dyn_tag_slot, dyn_tag)) return false;
    }
    if (local->is_list) {
      if (list_len < 0) {
        list_len = ny_native_nir_emit_runtime_call(
            b, "rt_native_tbuf_len", value, -1, -1, 1, 0);
        if (list_len < 0)
          return ny_native_nir_fail(
              b, "native NYIR lower: list assignment length query failed in %s",
              b->current_fn_name ? b->current_fn_name : "<unknown>");
      }
      if (local->list_len_slot < 0)
        local->list_len_slot = b->next_local_slot++;
      size_t length_before = b->nyir.len;
      nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_STORE_LOCAL,
                                         .dst = -1, .a = list_len, .b = -1,
                                         .imm = local->list_len_slot});
      if (b->nyir.len == length_before)
        return ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
    }
  }
  return true;
}

static int ny_native_nir_load_local_value(ny_native_nir_builder_t *b,
                                          int slot) {
  int v = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_LOAD_LOCAL,
                                               .dst = -1,
                                               .a = -1,
                                               .b = -1,
                                               .imm = slot});
  if (v < 0) {
    ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
    return v;
  }
  ny_native_nir_local_t *local = ny_native_nir_find_local_slot(b, slot);
  if (local && local->buffer_byte_len > 0 &&
      !ny_native_nir_record_alloc_fact(b, v, local->buffer_byte_len))
    return -1;
  if (local && local->fin_bound > 0 &&
      !ny_native_nir_record_fin_fact(b, v, local->fin_bound))
    return -1;
  if (local && local->dyn_str_len_slot >= 0 && local->dyn_tag_slot >= 0) {
    int length = ny_native_nir_load_local_value(b, local->dyn_str_len_slot);
    int tag = ny_native_nir_load_local_value(b, local->dyn_tag_slot);
    if (length < 0 || tag < 0 ||
        !ny_native_nir_record_dyn_fact(b, v, NY_NATIVE_NIR_FACT_DYN_STR_LEN, length) ||
        !ny_native_nir_record_dyn_fact(b, v, NY_NATIVE_NIR_FACT_DYN_TAG, tag)) return -1;
  }
  if (local && local->is_list) {
    int length = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_LOAD_LOCAL,
                                                   .dst = -1, .a = -1, .b = -1,
                                                   .imm = local->list_len_slot});
    if (length < 0 || !ny_native_nir_record_list_len_fact(b, v, length)) {
      ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
      return -1;
    }
  }
  return v;
}

static int ny_native_nir_emit_is_zero(ny_native_nir_builder_t *b, int value) {
  int zero = ny_native_nir_emit_const(b, 0);
  if (zero < 0)
    return -1;
  int v = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_CMP_I64,
                                               .dst = -1,
                                               .a = value,
                                               .b = zero,
                                               .cmp = NYIR_CMP_EQ});
  if (v < 0)
    ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
  return v;
}

static int ny_native_nir_lower_logical(ny_native_nir_builder_t *b,
                                       const expr_t *left,
                                       const expr_t *right,
                                       bool is_or);
static int ny_native_nir_lower_ternary(ny_native_nir_builder_t *b,
                                       const expr_t *cond,
                                       const expr_t *true_expr,
                                       const expr_t *false_expr);
static bool ny_native_nir_lower_match(ny_native_nir_builder_t *b,
                                      const stmt_t *s);

static int ny_native_nir_lower_expr(ny_native_nir_builder_t *b, const expr_t *e);
static int ny_native_nir_lower_binary(ny_native_nir_builder_t *b,
                                   const expr_t *e);
static int ny_native_nir_lower_call(ny_native_nir_builder_t *b,
                                 const expr_t *e);

#define NY_NATIVE_ASM_MAX_OPERANDS 32
#define NY_NATIVE_ASM_MAX_TOKEN 96
#define NY_NATIVE_ASM_MAX_TEMPLATE 4096

typedef struct {
  bool output;
  bool input;
  bool clobber;
  bool initialized;
  bool memory;
  bool immediate;
  int match;
  int value;
  unsigned bits;
  char fixed[16];
} ny_native_asm_operand_t;

typedef struct {
  ny_native_asm_operand_t operands[NY_NATIVE_ASM_MAX_OPERANDS];
  size_t count;
  int result;
} ny_native_asm_state_t;

#include "lower/lower_special.h"

#include "lower/lower_arith.h"

#include "lower/lower_call.h"

#include "lower/lower_expr.h"

#include "lower/lower_stmt.h"
