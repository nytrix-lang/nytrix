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

/* AST-to-NYIR lowering, extern discovery, optimized construction, dumps, and
 * metadata summaries. Execution and target emission live in other modules. */

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

typedef struct {
  const char *name;
  int slot;
  bool is_f64;
  bool is_f32;
} ny_native_nir_local_t;

typedef struct {
  int head_label;
  int continue_label;
  int end_label;
} ny_native_nir_loop_frame_t;

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
  /* Non-zero if the function returns an aggregate by value. */
  uint32_t ret_aggregate_size;
  ny_sysv_agg_class_t ret_aggregate_classes[2];
  /* Per-argument byval sizes; 0 = scalar, >0 = aggregate of that byte size. */
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
  /* Dedup: identical redeclarations are silently accepted. */
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
  ny_native_nir_loop_frame_t *loop_frames;
  size_t loop_depth;
  size_t loop_cap;
  bool emitted_return;
  /* Function return type is carried on NYIR_RET so type constraints survive
   * control-flow joins that contain no floating-point arithmetic themselves. */
  unsigned return_flags;
  const ny_extern_table_t *externs;
  const program_t *prog;
  const char *profile_name;
  int opt_level;
  char *err;
  size_t err_len;
} ny_native_nir_builder_t;

static bool ny_native_nir_fail(ny_native_nir_builder_t *b, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void ny_native_nir_builder_dispose(ny_native_nir_builder_t *b) {
  if (!b)
    return;
  free(b->locals);
  free(b->loop_frames);
  b->locals = NULL;
  b->loop_frames = NULL;
  b->local_count = 0;
  b->local_cap = 0;
  b->loop_depth = 0;
  b->loop_cap = 0;
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

static bool ny_native_type_name_is_f64(const char *name) {
  return name && (strcmp(name, "f64") == 0 || strcmp(name, "float") == 0);
}

static bool ny_native_type_name_is_f32(const char *name) {
  return name && (strcmp(name, "f32") == 0 || strcmp(name, "float32") == 0);
}

static bool ny_native_nir_set_param_types(ny_native_nir_builder_t *b,
                                              const stmt_t *fn) {
  if (!b || !fn || fn->kind != NY_S_FUNC)
    return false;
  size_t count = fn->as.fn.params.len;
  if (!count)
    return true;
  b->nyir.param_types = calloc(count, sizeof(*b->nyir.param_types));
  if (!b->nyir.param_types)
    return ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
  b->nyir.param_count = count;
  for (size_t i = 0; i < count; ++i) {
    const char *type = fn->as.fn.params.data[i].type;
    b->nyir.param_types[i] = ny_native_type_name_is_f64(type)
                                 ? NYIR_PARAM_F64
                                 : ny_native_type_name_is_f32(type)
                                       ? NYIR_PARAM_F32
                                       : NYIR_PARAM_I64;
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
    ny_native_nir_builder_t *b, const char *name, bool is_f64, bool is_f32) {
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
  l->name = name;
  l->slot = b->next_local_slot++;
  l->is_f64 = is_f64;
  l->is_f32 = is_f32;
  b->local_count++;
  return l;
}

static ny_native_nir_local_t *ny_native_nir_bind_local(ny_native_nir_builder_t *b,
                                                       const char *name) {
  return ny_native_nir_bind_local_typed(b, name, false, false);
}

static ny_native_nir_local_t *ny_native_nir_add_local(ny_native_nir_builder_t *b,
                                                      const char *name) {
  if (!name || !name[0] || strcmp(name, "_") == 0)
    return NULL;
  ny_native_nir_local_t *old = ny_native_nir_find_local(b, name);
  return old ? old : ny_native_nir_bind_local(b, name);
}

/* Compact NYIR emit helpers: one push path for control/value forms. */
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

static int ny_native_nir_emit_const(ny_native_nir_builder_t *b, int64_t value) {
  return ny_native_nir_push_val(b, NYIR_CONST_I64, -1, -1, value, NULL);
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
static bool ny_native_nir_emit_store_f64(ny_native_nir_builder_t *b, int addr,
                                         int value) {
  size_t before = b->nyir.len;
  nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_STORE_I64,
                                     .dst = -1, .a = addr, .b = -1, .c = value,
                                     .flags = NYIR_INST_F_MEM_F64});
  return b->nyir.len != before || ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
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

static bool ny_native_nir_expr_is_f64(ny_native_nir_builder_t *b, const expr_t *e) {
  if (!e)
    return false;
  switch (e->kind) {
  case NY_E_LITERAL:
    return e->as.literal.kind == NY_LIT_FLOAT;
  case NY_E_IDENT: {
    ny_native_nir_local_t *l = ny_native_nir_find_local(b, e->as.ident.name);
    return l && l->is_f64;
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
  case NY_E_CALL:
    if (e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT) {
      const char *leaf = ny_native_leaf_name(e->as.call.callee->as.ident.name);
      if (leaf && (strcmp(leaf, "float") == 0 ||
                   strcmp(leaf, "f64buf_load") == 0 ||
                   strcmp(leaf, "__flt_sqrt") == 0))
        return true;
    }
    if (e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT && b && b->prog) {
      const char *name = e->as.call.callee->as.ident.name;
      for (size_t i = 0; i < b->prog->body.len; ++i) {
        const stmt_t *s = b->prog->body.data[i];
        if (s && s->kind == NY_S_FUNC && s->as.fn.name &&
            strcmp(s->as.fn.name, name) == 0)
          return ny_native_type_name_is_f64(s->as.fn.return_type);
      }
    }
    return false;
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
    return l && l->is_f32;
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
    if (e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT && b && b->prog) {
      const char *name = e->as.call.callee->as.ident.name;
      for (size_t i = 0; i < b->prog->body.len; ++i) {
        const stmt_t *s = b->prog->body.data[i];
        if (s && s->kind == NY_S_FUNC && s->as.fn.name &&
            strcmp(s->as.fn.name, name) == 0)
          return ny_native_type_name_is_f32(s->as.fn.return_type);
      }
    }
    return false;
  default:
    return false;
  }
}

static const stmt_t *ny_native_nir_find_user_function(ny_native_nir_builder_t *b,
                                                     const char *name) {
  if (!b || !b->prog || !name)
    return NULL;
  for (size_t i = 0; i < b->prog->body.len; ++i) {
    const stmt_t *s = b->prog->body.data[i];
    if (s && s->kind == NY_S_FUNC && s->as.fn.name &&
        strcmp(s->as.fn.name, name) == 0)
      return s;
  }
  return NULL;
}

static const expr_t *ny_native_nir_find_top_level_value(
    const ny_native_nir_builder_t *b, const char *name) {
  if (!b || !b->prog || !name)
    return NULL;
  for (size_t i = 0; i < b->prog->body.len; ++i) {
    const stmt_t *s = b->prog->body.data[i];
    if (!s || s->kind != NY_S_VAR)
      continue;
    for (size_t n = 0; n < s->as.var.names.len && n < s->as.var.exprs.len; ++n)
      if (s->as.var.names.data[n] &&
          strcmp(s->as.var.names.data[n], name) == 0)
        return s->as.var.exprs.data[n];
  }
  return NULL;
}

static bool ny_native_nir_store_local_value(ny_native_nir_builder_t *b,
                                            int slot, int value) {
  size_t before = b->nyir.len;
  nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_STORE_LOCAL,
                                       .dst = -1,
                                       .a = value,
                                       .b = -1,
                                       .imm = slot});
  return b->nyir.len != before ||
         ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
}

static int ny_native_nir_load_local_value(ny_native_nir_builder_t *b,
                                          int slot) {
  int v = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_LOAD_LOCAL,
                                               .dst = -1,
                                               .a = -1,
                                               .b = -1,
                                               .imm = slot});
  if (v < 0)
    ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
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

static char *ny_native_asm_trim(char *s) {
  while (*s && isspace((unsigned char)*s))
    s++;
  char *end = s + strlen(s);
  while (end > s && isspace((unsigned char)end[-1]))
    *--end = '\0';
  return s;
}

static size_t ny_native_asm_split(char *s, char delimiter, char **parts,
                                  size_t cap) {
  size_t count = 0;
  int square = 0, round = 0, curly = 0;
  if (cap)
    parts[count++] = s;
  for (char *p = s; *p; ++p) {
    if (*p == '[') square++;
    else if (*p == ']') square--;
    else if (*p == '(') round++;
    else if (*p == ')') round--;
    else if (*p == '{') curly++;
    else if (*p == '}') curly--;
    else if (*p == delimiter && square == 0 && round == 0 && curly == 0) {
      *p = '\0';
      if (count < cap)
        parts[count++] = p + 1;
    }
  }
  return count;
}

static bool ny_native_asm_constraint_class(const char *s, bool *memory,
                                           bool *immediate, unsigned *bits,
                                           char fixed[16], int *match) {
  *memory = false;
  *immediate = false;
  *bits = 64;
  *match = -1;
  fixed[0] = '\0';
  while (*s == '=' || *s == '+' || *s == '&' || *s == '%' || *s == '*' ||
         *s == '?' || *s == '!')
    s++;
  if (*s == '{') {
    const char *end = strchr(s + 1, '}');
    if (!end || end == s + 1 || (size_t)(end - s - 1) >= 16)
      return false;
    memcpy(fixed, s + 1, (size_t)(end - s - 1));
    fixed[end - s - 1] = '\0';
    for (char *p = fixed; *p; ++p)
      *p = (char)tolower((unsigned char)*p);
    if (fixed[0] == 'w')
      *bits = 32;
    return true;
  }
  if (isdigit((unsigned char)*s)) {
    char *end = NULL;
    long value = strtol(s, &end, 10);
    if (end == s || value < 0 || value >= NY_NATIVE_ASM_MAX_OPERANDS)
      return false;
    *match = (int)value;
    return true;
  }
  if (!*s)
    return false;
  if (*s == 'm' || *s == 'Q' || *s == 'U')
    *memory = true;
  else if (*s == 'i' || *s == 'n' || (*s >= 'I' && *s <= 'P') || *s == 'S')
    *immediate = true;
  if (*s == 'w')
    *bits = 32;
  return true;
}

static bool ny_native_asm_parse_constraints(ny_native_nir_builder_t *b,
                                             const expr_t *e,
                                             ny_native_asm_state_t *state) {
  char buf[2048];
  const char *constraints = e->as.as_asm.constraints ? e->as.as_asm.constraints : "";
  size_t len = strlen(constraints);
  if (len >= sizeof(buf))
    return ny_native_nir_fail(b, "native NYIR asm: constraints are too long");
  memcpy(buf, constraints, len + 1);
  char *tokens[NY_NATIVE_ASM_MAX_OPERANDS];
  size_t count = len ? ny_native_asm_split(buf, ',', tokens, NY_NATIVE_ASM_MAX_OPERANDS) : 0;
  if (count >= NY_NATIVE_ASM_MAX_OPERANDS && strchr(tokens[count - 1], ','))
    return ny_native_nir_fail(b, "native NYIR asm: too many operands");
  memset(state, 0, sizeof(*state));
  state->count = count;
  state->result = -1;
  size_t arg = 0;
  for (size_t i = 0; i < count; ++i) {
    char *token = ny_native_asm_trim(tokens[i]);
    char *alt = strchr(token, '|');
    if (alt)
      *alt = '\0';
    ny_native_asm_operand_t *op = &state->operands[i];
    op->match = -1;
    op->value = -1;
    op->clobber = token[0] == '~';
    if (op->clobber)
      continue;
    op->output = strchr(token, '=') != NULL || strchr(token, '+') != NULL;
    op->input = !op->output || strchr(token, '+') != NULL;
    if (!ny_native_asm_constraint_class(token, &op->memory, &op->immediate,
                                        &op->bits, op->fixed, &op->match))
      return ny_native_nir_fail(b, "native NYIR asm: unsupported constraint '%s'", token);
    if (op->match >= 0)
      op->input = true;
    if (op->output && state->result < 0)
      state->result = (int)i;
    if (op->input) {
      if (arg >= e->as.as_asm.args.len)
        return ny_native_nir_fail(b, "native NYIR asm: constraint/input count mismatch");
      int value = ny_native_nir_lower_expr(b, e->as.as_asm.args.data[arg++]);
      if (value < 0)
        return false;
      op->value = value;
      op->initialized = true;
      if (op->match >= 0) {
        if ((size_t)op->match >= count || !state->operands[op->match].output)
          return ny_native_nir_fail(b, "native NYIR asm: invalid matching constraint %d", op->match);
        state->operands[op->match].value = value;
        state->operands[op->match].initialized = true;
      }
    }
  }
  if (arg != e->as.as_asm.args.len)
    return ny_native_nir_fail(b, "native NYIR asm: %zu arguments are not described by constraints",
                              e->as.as_asm.args.len - arg);
  return true;
}

static int ny_native_asm_emit_binop(ny_native_nir_builder_t *b, nyir_op_t op,
                                    int a, int c) {
  int value = nyir_emit(&b->nyir, (nyir_inst_t){.op = op, .dst = -1, .a = a,
                                                    .b = c, .c = -1});
  if (value < 0)
    ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
  return value;
}

static int ny_native_asm_emit_cmp(ny_native_nir_builder_t *b, nyir_cmp_t cmp,
                                  int a, int c) {
  int value = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_CMP_I64,
                                                    .dst = -1, .a = a, .b = c,
                                                    .cmp = cmp});
  if (value < 0)
    ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
  return value;
}

static int ny_native_asm_mask32(ny_native_nir_builder_t *b, int value) {
  int mask = ny_native_nir_emit_const(b, 0xffffffffLL);
  return mask < 0 ? -1 : ny_native_asm_emit_binop(b, NYIR_AND_I64, value, mask);
}

static int ny_native_asm_lsr(ny_native_nir_builder_t *b, int value, int shift) {
  int mask = ny_native_nir_emit_const(b, 63);
  int zero = ny_native_nir_emit_const(b, 0);
  int sixty_four = ny_native_nir_emit_const(b, 64);
  if (mask < 0 || zero < 0 || sixty_four < 0)
    return -1;
  int s = ny_native_asm_emit_binop(b, NYIR_AND_I64, shift, mask);
  int is_zero = ny_native_asm_emit_cmp(b, NYIR_CMP_EQ, s, zero);
  int slot = ny_native_nir_temp_slot(b);
  int zero_label = b->next_label++;
  int end_label = b->next_label++;
  if (s < 0 || is_zero < 0 || !ny_native_nir_emit_br_if(b, is_zero, zero_label))
    return -1;
  int sar = ny_native_asm_emit_binop(b, NYIR_SAR_I64, value, s);
  int sign = ny_native_asm_emit_cmp(b, NYIR_CMP_LT, value, zero);
  int inverse = ny_native_asm_emit_binop(b, NYIR_SUB_I64, sixty_four, s);
  int bias = ny_native_asm_emit_binop(b, NYIR_SHL_I64, sign, inverse);
  int result = ny_native_asm_emit_binop(b, NYIR_ADD_I64, sar, bias);
  if (result < 0 || !ny_native_nir_store_local_value(b, slot, result) ||
      !ny_native_nir_emit_br(b, end_label) ||
      !ny_native_nir_emit_label(b, zero_label) ||
      !ny_native_nir_store_local_value(b, slot, value) ||
      !ny_native_nir_emit_label(b, end_label))
    return -1;
  return ny_native_nir_load_local_value(b, slot);
}

static int ny_native_asm_sdiv(ny_native_nir_builder_t *b, int a, int c) {
  int zero = ny_native_nir_emit_const(b, 0);
  int min = ny_native_nir_emit_const(b, INT64_MIN);
  int neg_one = ny_native_nir_emit_const(b, -1);
  if (zero < 0 || min < 0 || neg_one < 0)
    return -1;
  int div_zero = ny_native_asm_emit_cmp(b, NYIR_CMP_EQ, c, zero);
  int lhs_min = ny_native_asm_emit_cmp(b, NYIR_CMP_EQ, a, min);
  int rhs_neg_one = ny_native_asm_emit_cmp(b, NYIR_CMP_EQ, c, neg_one);
  int overflow = ny_native_asm_emit_binop(b, NYIR_AND_I64, lhs_min, rhs_neg_one);
  int slot = ny_native_nir_temp_slot(b);
  int zero_label = b->next_label++;
  int overflow_label = b->next_label++;
  int end_label = b->next_label++;
  if (div_zero < 0 || overflow < 0 ||
      !ny_native_nir_emit_br_if(b, div_zero, zero_label) ||
      !ny_native_nir_emit_br_if(b, overflow, overflow_label))
    return -1;
  int result = ny_native_asm_emit_binop(b, NYIR_DIV_I64, a, c);
  if (result < 0 || !ny_native_nir_store_local_value(b, slot, result) ||
      !ny_native_nir_emit_br(b, end_label) ||
      !ny_native_nir_emit_label(b, zero_label) ||
      !ny_native_nir_store_local_value(b, slot, zero) ||
      !ny_native_nir_emit_br(b, end_label) ||
      !ny_native_nir_emit_label(b, overflow_label) ||
      !ny_native_nir_store_local_value(b, slot, min) ||
      !ny_native_nir_emit_label(b, end_label))
    return -1;
  return ny_native_nir_load_local_value(b, slot);
}

static int ny_native_asm_uge(ny_native_nir_builder_t *b, int a, int c) {
  int sign = ny_native_nir_emit_const(b, INT64_MIN);
  if (sign < 0)
    return -1;
  int ax = ny_native_asm_emit_binop(b, NYIR_XOR_I64, a, sign);
  int cx = ny_native_asm_emit_binop(b, NYIR_XOR_I64, c, sign);
  return ax < 0 || cx < 0 ? -1 : ny_native_asm_emit_cmp(b, NYIR_CMP_GE, ax, cx);
}

static int ny_native_asm_udiv(ny_native_nir_builder_t *b, int a, int c) {
  int zero = ny_native_nir_emit_const(b, 0);
  int one = ny_native_nir_emit_const(b, 1);
  if (zero < 0 || one < 0)
    return -1;
  int div_zero = ny_native_asm_emit_cmp(b, NYIR_CMP_EQ, c, zero);
  int divisor_high = ny_native_asm_emit_cmp(b, NYIR_CMP_LT, c, zero);
  int dividend_high = ny_native_asm_emit_cmp(b, NYIR_CMP_LT, a, zero);
  int slot = ny_native_nir_temp_slot(b);
  int zero_label = b->next_label++;
  int divisor_high_label = b->next_label++;
  int dividend_high_label = b->next_label++;
  int end_label = b->next_label++;
  if (div_zero < 0 || divisor_high < 0 || dividend_high < 0 ||
      !ny_native_nir_emit_br_if(b, div_zero, zero_label) ||
      !ny_native_nir_emit_br_if(b, divisor_high, divisor_high_label) ||
      !ny_native_nir_emit_br_if(b, dividend_high, dividend_high_label))
    return -1;
  int result = ny_native_asm_emit_binop(b, NYIR_DIV_I64, a, c);
  if (result < 0 || !ny_native_nir_store_local_value(b, slot, result) ||
      !ny_native_nir_emit_br(b, end_label) ||
      !ny_native_nir_emit_label(b, divisor_high_label))
    return -1;
  result = ny_native_asm_uge(b, a, c);
  if (result < 0 || !ny_native_nir_store_local_value(b, slot, result) ||
      !ny_native_nir_emit_br(b, end_label) ||
      !ny_native_nir_emit_label(b, dividend_high_label))
    return -1;
  int half = ny_native_asm_lsr(b, a, one);
  int half_q = half < 0 ? -1 : ny_native_asm_emit_binop(b, NYIR_DIV_I64, half, c);
  int q = half_q < 0 ? -1 : ny_native_asm_emit_binop(b, NYIR_SHL_I64, half_q, one);
  int product = q < 0 ? -1 : ny_native_asm_emit_binop(b, NYIR_MUL_I64, q, c);
  int remainder = product < 0 ? -1 : ny_native_asm_emit_binop(b, NYIR_SUB_I64, a, product);
  int carry = remainder < 0 ? -1 : ny_native_asm_uge(b, remainder, c);
  result = carry < 0 ? -1 : ny_native_asm_emit_binop(b, NYIR_ADD_I64, q, carry);
  if (result < 0 || !ny_native_nir_store_local_value(b, slot, result) ||
      !ny_native_nir_emit_br(b, end_label) ||
      !ny_native_nir_emit_label(b, zero_label) ||
      !ny_native_nir_store_local_value(b, slot, zero) ||
      !ny_native_nir_emit_label(b, end_label))
    return -1;
  return ny_native_nir_load_local_value(b, slot);
}

static bool ny_native_asm_ref(const ny_native_asm_state_t *state, const char *text,
                              int *index, unsigned *bits) {
  char buf[NY_NATIVE_ASM_MAX_TOKEN];
  size_t len = strlen(text);
  if (len >= sizeof(buf))
    return false;
  memcpy(buf, text, len + 1);
  char *s = ny_native_asm_trim(buf);
  *bits = 64;
  if ((s[0] == 'w' || s[0] == 'x') && isdigit((unsigned char)s[1])) {
    char reg[16];
    snprintf(reg, sizeof(reg), "%s", s);
    for (char *p = reg; *p; ++p)
      *p = (char)tolower((unsigned char)*p);
    for (size_t i = 0; i < state->count; ++i)
      if (state->operands[i].fixed[0] && strcmp(state->operands[i].fixed, reg) == 0) {
        *index = (int)i;
        *bits = reg[0] == 'w' ? 32 : 64;
        return true;
      }
  }
  const char *p = strchr(s, '$');
  if (!p)
    p = strchr(s, '%');
  if (!p)
    return false;
  p++;
  if (*p == '{')
    p++;
  if (*p == 'w' || *p == 'x') {
    *bits = *p == 'w' ? 32 : 64;
    p++;
  }
  while (*p && !isdigit((unsigned char)*p))
    p++;
  if (!isdigit((unsigned char)*p))
    return false;
  char *end = NULL;
  long value = strtol(p, &end, 10);
  if (value < 0 || (size_t)value >= state->count)
    return false;
  const char *modifier = strchr(end, ':');
  if (modifier && modifier[1] == 'w')
    *bits = 32;
  *index = (int)value;
  return true;
}

static int ny_native_asm_source(ny_native_nir_builder_t *b,
                                const ny_native_asm_state_t *state,
                                const char *text, unsigned *bits) {
  char buf[NY_NATIVE_ASM_MAX_TOKEN];
  size_t len = strlen(text);
  if (len >= sizeof(buf)) {
    ny_native_nir_fail(b, "native NYIR asm: operand is too long");
    return -1;
  }
  memcpy(buf, text, len + 1);
  char *s = ny_native_asm_trim(buf);
  if (strcasecmp(s, "xzr") == 0 || strcasecmp(s, "wzr") == 0) {
    *bits = tolower((unsigned char)s[0]) == 'w' ? 32 : 64;
    return ny_native_nir_emit_const(b, 0);
  }
  int index = -1;
  if (ny_native_asm_ref(state, s, &index, bits)) {
    const ny_native_asm_operand_t *op = &state->operands[index];
    if (!op->initialized) {
      ny_native_nir_fail(b, "native NYIR asm: operand %d is read before it is written", index);
      return -1;
    }
    int value = op->value;
    if (*bits == 32 || op->bits == 32)
      value = ny_native_asm_mask32(b, value);
    return value;
  }
  if (*s == '#')
    s++;
  errno = 0;
  char *end = NULL;
  long long value = strtoll(s, &end, 0);
  if (!errno && end != s && *ny_native_asm_trim(end) == '\0') {
    *bits = 64;
    return ny_native_nir_emit_const(b, (int64_t)value);
  }
  ny_native_nir_fail(b, "native NYIR asm: unsupported operand '%s'", text);
  return -1;
}

static bool ny_native_asm_assign(ny_native_nir_builder_t *b,
                                 ny_native_asm_state_t *state, const char *text,
                                 int value) {
  int index = -1;
  unsigned bits = 64;
  if (!ny_native_asm_ref(state, text, &index, &bits))
    return ny_native_nir_fail(b, "native NYIR asm: destination '%s' is not constrained", text);
  if (bits == 32 || state->operands[index].bits == 32) {
    value = ny_native_asm_mask32(b, value);
    if (value < 0)
      return false;
  }
  state->operands[index].value = value;
  state->operands[index].initialized = true;
  return true;
}

static int ny_native_asm_address(ny_native_nir_builder_t *b,
                                 const ny_native_asm_state_t *state,
                                 const char *text, unsigned *width) {
  char buf[NY_NATIVE_ASM_MAX_TOKEN * 2];
  size_t len = strlen(text);
  if (len >= sizeof(buf)) {
    ny_native_nir_fail(b, "native NYIR asm: memory operand is too long");
    return -1;
  }
  memcpy(buf, text, len + 1);
  char *s = ny_native_asm_trim(buf);
  if (*s != '[')
    return ny_native_asm_source(b, state, s, width);
  char *end = strrchr(s, ']');
  if (!end)
    return ny_native_nir_fail(b, "native NYIR asm: malformed memory operand '%s'", text), -1;
  *end = '\0';
  char *parts[3];
  size_t count = ny_native_asm_split(s + 1, ',', parts, 3);
  unsigned bits = 64;
  int base = ny_native_asm_source(b, state, parts[0], &bits);
  if (base < 0)
    return -1;
  if (count > 1) {
    int offset = ny_native_asm_source(b, state, parts[1], &bits);
    if (offset < 0)
      return -1;
    base = ny_native_asm_emit_binop(b, NYIR_ADD_I64, base, offset);
  }
  *width = 64;
  return base;
}

static bool ny_native_asm_exec(ny_native_nir_builder_t *b,
                               ny_native_asm_state_t *state, const char *code) {
  char buf[NY_NATIVE_ASM_MAX_TEMPLATE];
  size_t len = strlen(code);
  if (len >= sizeof(buf))
    return ny_native_nir_fail(b, "native NYIR asm: template is too long");
  memcpy(buf, code, len + 1);
  for (char *p = buf; *p; ++p)
    if (*p == ';')
      *p = '\n';
  char *save = NULL;
  for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
    char *comment = strstr(line, "//");
    if (comment)
      *comment = '\0';
    line = ny_native_asm_trim(line);
    if (!*line)
      continue;
    char *space = line;
    while (*space && !isspace((unsigned char)*space))
      space++;
    char mnemonic[16];
    size_t mlen = (size_t)(space - line);
    if (mlen == 0 || mlen >= sizeof(mnemonic))
      return ny_native_nir_fail(b, "native NYIR asm: invalid instruction '%s'", line);
    memcpy(mnemonic, line, mlen);
    mnemonic[mlen] = '\0';
    for (char *p = mnemonic; *p; ++p)
      *p = (char)tolower((unsigned char)*p);
    char *operand_text = ny_native_asm_trim(space);
    char *args[4] = {0};
    size_t argc = *operand_text ? ny_native_asm_split(operand_text, ',', args, 4) : 0;
    for (size_t i = 0; i < argc; ++i)
      args[i] = ny_native_asm_trim(args[i]);
    if (strcmp(mnemonic, "nop") == 0 || strcmp(mnemonic, "yield") == 0 ||
        strcmp(mnemonic, "wfe") == 0 || strcmp(mnemonic, "wfi") == 0 ||
        strcmp(mnemonic, "sev") == 0 || strcmp(mnemonic, "isb") == 0 ||
        strcmp(mnemonic, "dmb") == 0 || strcmp(mnemonic, "dsb") == 0)
      continue;
    if (strcmp(mnemonic, "mov") == 0 && argc == 2) {
      unsigned bits = 64;
      int value = ny_native_asm_source(b, state, args[1], &bits);
      if (value < 0 || !ny_native_asm_assign(b, state, args[0], value))
        return false;
      continue;
    }
    if (strcmp(mnemonic, "mvn") == 0 && argc == 2) {
      unsigned bits = 64;
      int value = ny_native_asm_source(b, state, args[1], &bits);
      int all = ny_native_nir_emit_const(b, -1);
      value = value < 0 || all < 0 ? -1 : ny_native_asm_emit_binop(b, NYIR_XOR_I64, value, all);
      if (value < 0 || !ny_native_asm_assign(b, state, args[0], value))
        return false;
      continue;
    }
    if (strcmp(mnemonic, "ldr") == 0 && argc == 2) {
      unsigned width = 64;
      int address = ny_native_asm_address(b, state, args[1], &width);
      if (address < 0 || width != 64)
        return ny_native_nir_fail(b, "native NYIR asm: only 64-bit ldr is supported");
      int value = ny_native_nir_emit_load_i64(b, address);
      if (value < 0 || !ny_native_asm_assign(b, state, args[0], value))
        return false;
      continue;
    }
    if (strcmp(mnemonic, "str") == 0 && argc == 2) {
      unsigned bits = 64, width = 64;
      int value = ny_native_asm_source(b, state, args[0], &bits);
      int address = value < 0 ? -1 : ny_native_asm_address(b, state, args[1], &width);
      if (value < 0 || address < 0 || bits != 64 || width != 64)
        return ny_native_nir_fail(b, "native NYIR asm: only 64-bit str is supported");
      if (!ny_native_nir_emit_store_i64(b, address, value))
        return false;
      continue;
    }
    bool shift = strcmp(mnemonic, "lsl") == 0 || strcmp(mnemonic, "lsr") == 0 ||
                 strcmp(mnemonic, "asr") == 0;
    bool binary = strcmp(mnemonic, "add") == 0 || strcmp(mnemonic, "sub") == 0 ||
                  strcmp(mnemonic, "and") == 0 || strcmp(mnemonic, "eor") == 0 ||
                  strcmp(mnemonic, "orr") == 0 || strcmp(mnemonic, "mul") == 0 ||
                  strcmp(mnemonic, "sdiv") == 0 || strcmp(mnemonic, "udiv") == 0 ||
                  strcmp(mnemonic, "bic") == 0 || shift;
    if (binary && (argc == 2 || argc == 3)) {
      const char *lhs_text = argc == 3 ? args[1] : args[0];
      const char *rhs_text = argc == 3 ? args[2] : args[1];
      unsigned lhs_bits = 64, rhs_bits = 64;
      int lhs = ny_native_asm_source(b, state, lhs_text, &lhs_bits);
      int rhs = lhs < 0 ? -1 : ny_native_asm_source(b, state, rhs_text, &rhs_bits);
      int value = -1;
      if (lhs < 0 || rhs < 0)
        return false;
      if (strcmp(mnemonic, "add") == 0) value = ny_native_asm_emit_binop(b, NYIR_ADD_I64, lhs, rhs);
      else if (strcmp(mnemonic, "sub") == 0) value = ny_native_asm_emit_binop(b, NYIR_SUB_I64, lhs, rhs);
      else if (strcmp(mnemonic, "and") == 0) value = ny_native_asm_emit_binop(b, NYIR_AND_I64, lhs, rhs);
      else if (strcmp(mnemonic, "eor") == 0) value = ny_native_asm_emit_binop(b, NYIR_XOR_I64, lhs, rhs);
      else if (strcmp(mnemonic, "orr") == 0) value = ny_native_asm_emit_binop(b, NYIR_OR_I64, lhs, rhs);
      else if (strcmp(mnemonic, "mul") == 0) value = ny_native_asm_emit_binop(b, NYIR_MUL_I64, lhs, rhs);
      else if (strcmp(mnemonic, "sdiv") == 0) value = ny_native_asm_sdiv(b, lhs, rhs);
      else if (strcmp(mnemonic, "udiv") == 0) value = ny_native_asm_udiv(b, lhs, rhs);
      else if (strcmp(mnemonic, "lsl") == 0) value = ny_native_asm_emit_binop(b, NYIR_SHL_I64, lhs, rhs);
      else if (strcmp(mnemonic, "asr") == 0) value = ny_native_asm_emit_binop(b, NYIR_SAR_I64, lhs, rhs);
      else if (strcmp(mnemonic, "lsr") == 0) value = ny_native_asm_lsr(b, lhs, rhs);
      else {
        int all = ny_native_nir_emit_const(b, -1);
        int inverted = all < 0 ? -1 : ny_native_asm_emit_binop(b, NYIR_XOR_I64, rhs, all);
        value = inverted < 0 ? -1 : ny_native_asm_emit_binop(b, NYIR_AND_I64, lhs, inverted);
      }
      if (value < 0 || !ny_native_asm_assign(b, state, args[0], value))
        return false;
      continue;
    }
    return ny_native_nir_fail(b, "native NYIR asm: unsupported AArch64 template instruction '%s'", mnemonic);
  }
  return true;
}

static int ny_native_nir_lower_aarch64_asm(ny_native_nir_builder_t *b,
                                            const expr_t *e) {
  ny_native_asm_state_t state;
  if (!ny_native_asm_parse_constraints(b, e, &state) ||
      !ny_native_asm_exec(b, &state, e->as.as_asm.code ? e->as.as_asm.code : ""))
    return -1;
  if (state.result < 0)
    return ny_native_nir_emit_const(b, 0);
  ny_native_asm_operand_t *result = &state.operands[state.result];
  if (!result->initialized)
    return ny_native_nir_fail(b, "native NYIR asm: output operand %d was not written", state.result), -1;
  return result->value;
}

static int ny_native_nir_lower_expr(ny_native_nir_builder_t *b, const expr_t *e) {
  if (!e) {
    ny_native_nir_fail(b, "native NYIR lower: missing expression");
    return -1;
  }
  switch (e->kind) {
  case NY_E_LITERAL:
    if (e->as.literal.kind == NY_LIT_BOOL)
      return ny_native_nir_emit_const(b, e->as.literal.as.b ? 1 : 0);
    if (e->as.literal.kind == NY_LIT_FLOAT)
      return ny_native_nir_emit_const_f64(b, e->as.literal.as.f);
    if (e->tok.kind == NY_T_NIL)
      return ny_native_nir_emit_const(b, 0);
    if (e->as.literal.kind == NY_LIT_STR) {
      /* C-string pointer via interned .Lnystr.N (appended into code blob). */
      const char *s = e->as.literal.as.s.data ? e->as.literal.as.s.data : "";
      size_t slen = e->as.literal.as.s.len;
      const char *sym = ny_native_strtab_intern(s, slen, NULL, 0);
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
      if (addr < 0)
        ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
      return addr;
    }
    if (e->as.literal.kind != NY_LIT_INT) {
      ny_native_nir_fail(
          b, "native NYIR lower: only int/bool/f64/nil/string literals are supported");
      return -1;
    }
    return ny_native_nir_emit_const(b, e->as.literal.as.i);
  case NY_E_IDENT: {
    ny_native_nir_local_t *l = ny_native_nir_find_local(b, e->as.ident.name);
    if (!l) {
      const expr_t *global = ny_native_nir_find_top_level_value(b, e->as.ident.name);
      if (global && global != e && global->kind == NY_E_LITERAL)
        return ny_native_nir_lower_expr(b, global);
    }
    if (!l) {
      int addr = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_ADDR_SYMBOL,
                                                      .dst = -1,
                                                      .a = -1,
                                                      .b = -1,
                                                      .imm = 0,
                                                      .symbol = e->as.ident.name});
      if (addr < 0)
        ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
      return addr;
    }
    int v = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_LOAD_LOCAL,
                                                 .dst = -1,
                                                 .a = -1,
                                                 .b = -1,
                                                 .imm = l->slot,
                                                 .symbol = l->name});
    if (v < 0)
      ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
    return v;
  }
  case NY_E_UNARY: {
    if (!e->as.unary.op || !e->as.unary.right) {
      ny_native_nir_fail(b, "native NYIR lower: malformed unary");
      return -1;
    }
    int rv = ny_native_nir_lower_expr(b, e->as.unary.right);
    if (rv < 0)
      return -1;
    if (strcmp(e->as.unary.op, "+") == 0)
      return rv;
    if (strcmp(e->as.unary.op, "-") == 0) {
      int zero = ny_native_nir_emit_const(b, 0);
      if (zero < 0)
        return -1;
      return nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SUB_I64,
                                                  .dst = -1,
                                                  .a = zero,
                                                  .b = rv});
    }
    if (strcmp(e->as.unary.op, "!") == 0) {
      int zero = ny_native_nir_emit_const(b, 0);
      if (zero < 0)
        return -1;
      return nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_CMP_I64,
                                                  .dst = -1,
                                                  .a = rv,
                                                  .b = zero,
                                                  .cmp = NYIR_CMP_EQ});
    }
    if (strcmp(e->as.unary.op, "~") == 0) {
      int mask = ny_native_nir_emit_const(b, -1);
      if (mask < 0)
        return -1;
      return nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_XOR_I64,
                                                  .dst = -1,
                                                  .a = rv,
                                                  .b = mask});
    }
    ny_native_nir_fail(b, "native NYIR lower: unsupported unary operator '%s'",
                       e->as.unary.op);
    return -1;
  }
  case NY_E_BINARY: {
    if (e->as.binary.op && strcmp(e->as.binary.op, "^") == 0) {
      int64_t folded = 0;
      if (ny_native_nir_fold_const_pow(e->as.binary.left,
                                       e->as.binary.right, &folded))
        return ny_native_nir_emit_const(b, folded);
      ny_native_nir_fail(
          b, "native NYIR lower: power requires non-negative integer constants without overflow");
      return -1;
    }
    nyir_op_t op = NYIR_NOP;
    nyir_cmp_t cmp = NYIR_CMP_EQ;
    bool is_cmp = ny_native_nir_cmp(e->as.binary.op, &cmp);
    if (!is_cmp && !ny_native_nir_binop(e->as.binary.op, &op)) {
      ny_native_nir_fail(b, "native NYIR lower: unsupported binary operator '%s'",
                         e->as.binary.op ? e->as.binary.op : "(null)");
      return -1;
    }
    bool expr_f32 = !ny_native_nir_expr_is_f64(b, e) &&
                    ny_native_nir_expr_is_f32(b, e);
    if (!is_cmp && expr_f32 &&
        (strcmp(e->as.binary.op, "+") == 0 || strcmp(e->as.binary.op, "-") == 0 ||
         strcmp(e->as.binary.op, "*") == 0 || strcmp(e->as.binary.op, "/") == 0)) {
      if (strcmp(e->as.binary.op, "+") == 0)
        op = NYIR_ADD_F32;
      else if (strcmp(e->as.binary.op, "-") == 0)
        op = NYIR_SUB_F32;
      else if (strcmp(e->as.binary.op, "*") == 0)
        op = NYIR_MUL_F32;
      else
        op = NYIR_DIV_F32;
    } else if (!is_cmp && ny_native_nir_expr_is_f64(b, e) &&
        (strcmp(e->as.binary.op, "+") == 0 || strcmp(e->as.binary.op, "-") == 0 ||
         strcmp(e->as.binary.op, "*") == 0 || strcmp(e->as.binary.op, "/") == 0)) {
      if (strcmp(e->as.binary.op, "+") == 0)
        op = NYIR_ADD_F64;
      else if (strcmp(e->as.binary.op, "-") == 0)
        op = NYIR_SUB_F64;
      else if (strcmp(e->as.binary.op, "*") == 0)
        op = NYIR_MUL_F64;
      else
        op = NYIR_DIV_F64;
    }
    bool left_f64 = ny_native_nir_expr_is_f64(b, e->as.binary.left);
    bool right_f64 = ny_native_nir_expr_is_f64(b, e->as.binary.right);
    bool left_f32 = ny_native_nir_expr_is_f32(b, e->as.binary.left);
    bool right_f32 = ny_native_nir_expr_is_f32(b, e->as.binary.right);
    int a = ny_native_nir_lower_expr(b, e->as.binary.left);
    int rhs = ny_native_nir_lower_expr(b, e->as.binary.right);
    if (a < 0 || rhs < 0)
      return -1;
    bool use_f64_cmp = is_cmp && ny_native_nir_expr_is_f64(b, e);
    bool use_f32_cmp = is_cmp && !use_f64_cmp && expr_f32;
    if ((!is_cmp && (op == NYIR_ADD_F32 || op == NYIR_SUB_F32 ||
                     op == NYIR_MUL_F32 || op == NYIR_DIV_F32)) ||
        use_f32_cmp) {
      if (!left_f32) {
        a = ny_native_nir_emit_i64_to_f32(b, a);
        if (a < 0)
          return -1;
      }
      if (!right_f32) {
        rhs = ny_native_nir_emit_i64_to_f32(b, rhs);
        if (rhs < 0)
          return -1;
      }
    } else if ((!is_cmp && (op == NYIR_ADD_F64 || op == NYIR_SUB_F64 ||
                     op == NYIR_MUL_F64 || op == NYIR_DIV_F64)) ||
        use_f64_cmp) {
      if (!left_f64) {
        a = ny_native_nir_emit_i64_to_f64(b, a);
        if (a < 0)
          return -1;
      }
      if (!right_f64) {
        rhs = ny_native_nir_emit_i64_to_f64(b, rhs);
        if (rhs < 0)
          return -1;
      }
    }
    int v = nyir_emit(&b->nyir, (nyir_inst_t){.op = use_f64_cmp ? NYIR_CMP_F64
                                                       : use_f32_cmp ? NYIR_CMP_F32
                                                       : is_cmp    ? NYIR_CMP_I64
                                                                   : op,
                                                 .dst = -1,
                                                 .a = a,
                                                 .b = rhs,
                                                 .cmp = cmp});
    if (v < 0)
      ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
    return v;
  }
  case NY_E_LOGICAL: {
    if (!e->as.logical.op ||
        (strcmp(e->as.logical.op, "&&") != 0 &&
         strcmp(e->as.logical.op, "||") != 0)) {
      ny_native_nir_fail(b, "native NYIR lower: unsupported logical operator '%s'",
                         e->as.logical.op ? e->as.logical.op : "(null)");
      return -1;
    }
    return ny_native_nir_lower_logical(
        b, e->as.logical.left, e->as.logical.right,
        strcmp(e->as.logical.op, "||") == 0);
  }
  case NY_E_TERNARY:
    return ny_native_nir_lower_ternary(b, e->as.ternary.cond,
                                       e->as.ternary.true_expr,
                                       e->as.ternary.false_expr);
  case NY_E_MEMCALL: {
    if (!e->as.memcall.target ||
        e->as.memcall.target->kind != NY_E_IDENT ||
        !e->as.memcall.target->as.ident.name || !e->as.memcall.name) {
      ny_native_nir_fail(
          b, "native NYIR lower: only namespace-qualified member calls are supported");
      return -1;
    }
    char qualified[512];
    int n = snprintf(qualified, sizeof(qualified), "%s.%s",
                     e->as.memcall.target->as.ident.name, e->as.memcall.name);
    if (n < 0 || (size_t)n >= sizeof(qualified)) {
      ny_native_nir_fail(b, "native NYIR lower: qualified call name is too long");
      return -1;
    }
    expr_t callee = {.kind = NY_E_IDENT, .tok = e->tok};
    callee.as.ident.name = qualified;
    expr_t call = *e;
    call.kind = NY_E_CALL;
    call.as.call.callee = &callee;
    call.as.call.args = e->as.memcall.args;
    return ny_native_nir_lower_expr(b, &call);
  }
  case NY_E_CALL: {
    if (!e->as.call.callee || e->as.call.callee->kind != NY_E_IDENT) {
      ny_native_nir_fail(b, "native NYIR lower: only direct calls are supported");
      return -1;
    }
    const char *name = e->as.call.callee->as.ident.name;
    const char *leaf = ny_native_leaf_name(name);
    if (leaf && strcmp(leaf, "intrinsic") == 0) {
      /* Capability-gated portable intrinsics: lower a small set to pure NYIR
       * (SWAR), not LLVM. Unknown names still fail explicitly. */
      if (e->as.call.args.len < 1 || !e->as.call.args.data[0].val ||
          e->as.call.args.data[0].val->kind != NY_E_LITERAL ||
          e->as.call.args.data[0].val->as.literal.kind != NY_LIT_STR) {
        ny_native_nir_fail(
            b, "native NYIR lower: intrinsic(...) first argument must be a string literal name");
        return -1;
      }
      const char *iname = e->as.call.args.data[0].val->as.literal.as.s.data;
      size_t iname_len = e->as.call.args.data[0].val->as.literal.as.s.len;
      if (!iname || iname_len == 0) {
        ny_native_nir_fail(b, "native NYIR lower: empty intrinsic name");
        return -1;
      }
      if (iname_len == 9 && memcmp(iname, "ctpop.i64", 9) == 0) {
        if (e->as.call.args.len != 2 || e->as.call.args.data[1].name) {
          ny_native_nir_fail(b, "native NYIR lower: ctpop.i64 expects one value");
          return -1;
        }
        int x = ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
        if (x < 0)
          return -1;
        /* SWAR popcount with logical shifts (SAR + clear high fill bits). */
        int c1 = ny_native_nir_emit_const(b, (int64_t)0x5555555555555555LL);
        int c2 = ny_native_nir_emit_const(b, (int64_t)0x3333333333333333LL);
        int c4 = ny_native_nir_emit_const(b, (int64_t)0x0f0f0f0f0f0f0f0fLL);
        int one = ny_native_nir_emit_const(b, 1);
        int two = ny_native_nir_emit_const(b, 2);
        int four = ny_native_nir_emit_const(b, 4);
        int eight = ny_native_nir_emit_const(b, 8);
        int sixteen = ny_native_nir_emit_const(b, 16);
        int thirtytwo = ny_native_nir_emit_const(b, 32);
        int mask7f = ny_native_nir_emit_const(b, 0x7f);
        /* Logical-shift masks: (1<<(64-n))-1 */
        int m1 = ny_native_nir_emit_const(b, (int64_t)0x7fffffffffffffffLL);
        int m2 = ny_native_nir_emit_const(b, (int64_t)0x3fffffffffffffffLL);
        int m4 = ny_native_nir_emit_const(b, (int64_t)0x0fffffffffffffffLL);
        int m8 = ny_native_nir_emit_const(b, (int64_t)0x00ffffffffffffffLL);
        int m16 = ny_native_nir_emit_const(b, (int64_t)0x0000ffffffffffffLL);
        int m32 = ny_native_nir_emit_const(b, (int64_t)0x00000000ffffffffLL);
        if (c1 < 0 || c2 < 0 || c4 < 0 || one < 0 || two < 0 || four < 0 ||
            eight < 0 || sixteen < 0 || thirtytwo < 0 || mask7f < 0 || m1 < 0 ||
            m2 < 0 || m4 < 0 || m8 < 0 || m16 < 0 || m32 < 0)
          return -1;
#define NY_LSHR(outv, src, sh, msk)                                            \
  do {                                                                         \
    int _t = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SAR_I64,        \
                                                  .dst = -1,                   \
                                                  .a = (src),                  \
                                                  .b = (sh)});                 \
    if (_t < 0)                                                                \
      return -1;                                                               \
    (outv) = nyir_emit(&b->nyir, (nyir_inst_t){                             \
                                      .op = NYIR_AND_I64,                    \
                                      .dst = -1,                               \
                                      .a = _t,                                 \
                                      .b = (msk)});                            \
    if ((outv) < 0)                                                            \
      return -1;                                                               \
  } while (0)
        int t;
        NY_LSHR(t, x, one, m1);
        t = nyir_emit(&b->nyir, (nyir_inst_t){
                                     .op = NYIR_AND_I64, .dst = -1, .a = t, .b = c1});
        if (t < 0)
          return -1;
        x = nyir_emit(&b->nyir, (nyir_inst_t){
                                     .op = NYIR_SUB_I64, .dst = -1, .a = x, .b = t});
        if (x < 0)
          return -1;
        int a = nyir_emit(&b->nyir, (nyir_inst_t){
                                         .op = NYIR_AND_I64, .dst = -1, .a = x, .b = c2});
        int b2;
        NY_LSHR(b2, x, two, m2);
        if (a < 0)
          return -1;
        b2 = nyir_emit(&b->nyir, (nyir_inst_t){
                                      .op = NYIR_AND_I64, .dst = -1, .a = b2, .b = c2});
        if (b2 < 0)
          return -1;
        x = nyir_emit(&b->nyir, (nyir_inst_t){
                                     .op = NYIR_ADD_I64, .dst = -1, .a = a, .b = b2});
        if (x < 0)
          return -1;
        NY_LSHR(t, x, four, m4);
        x = nyir_emit(&b->nyir, (nyir_inst_t){
                                     .op = NYIR_ADD_I64, .dst = -1, .a = x, .b = t});
        if (x < 0)
          return -1;
        x = nyir_emit(&b->nyir, (nyir_inst_t){
                                     .op = NYIR_AND_I64, .dst = -1, .a = x, .b = c4});
        if (x < 0)
          return -1;
        NY_LSHR(t, x, eight, m8);
        x = nyir_emit(&b->nyir, (nyir_inst_t){
                                     .op = NYIR_ADD_I64, .dst = -1, .a = x, .b = t});
        if (x < 0)
          return -1;
        NY_LSHR(t, x, sixteen, m16);
        x = nyir_emit(&b->nyir, (nyir_inst_t){
                                     .op = NYIR_ADD_I64, .dst = -1, .a = x, .b = t});
        if (x < 0)
          return -1;
        NY_LSHR(t, x, thirtytwo, m32);
        x = nyir_emit(&b->nyir, (nyir_inst_t){
                                     .op = NYIR_ADD_I64, .dst = -1, .a = x, .b = t});
        if (x < 0)
          return -1;
#undef NY_LSHR
        return nyir_emit(&b->nyir, (nyir_inst_t){
                                        .op = NYIR_AND_I64, .dst = -1, .a = x, .b = mask7f});
      }
      if ((iname_len == 8 && memcmp(iname, "cttz.i64", 8) == 0) ||
          (iname_len == 8 && memcmp(iname, "ctlz.i64", 8) == 0)) {
        int is_ctlz = (iname_len == 8 && memcmp(iname, "ctlz.i64", 8) == 0);
        /* cttz via ctpop((x & -x) - 1); zero input yields 64.
         * ctlz via cttz(bitreverse(x)). */
        if (e->as.call.args.len < 2 || e->as.call.args.len > 3 ||
            e->as.call.args.data[1].name) {
          ny_native_nir_fail(b, "native NYIR lower: cttz/ctlz.i64 expects a value");
          return -1;
        }
        int x = ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
        if (x < 0)
          return -1;
        if (is_ctlz) {
          /* Bitreverse x via SWAR so cttz on the result yields ctlz. */
          int bc1 = ny_native_nir_emit_const(b, (int64_t)0x5555555555555555LL);
          int bc2 = ny_native_nir_emit_const(b, (int64_t)0x3333333333333333LL);
          int bc4 = ny_native_nir_emit_const(b, (int64_t)0x0f0f0f0f0f0f0f0fLL);
          int bc8 = ny_native_nir_emit_const(b, (int64_t)0x00ff00ff00ff00ffLL);
          int bc16 = ny_native_nir_emit_const(b, (int64_t)0x0000ffff0000ffffLL);
          int b1 = ny_native_nir_emit_const(b, 1);
          int b2c = ny_native_nir_emit_const(b, 2);
          int b4c = ny_native_nir_emit_const(b, 4);
          int b8 = ny_native_nir_emit_const(b, 8);
          int b16 = ny_native_nir_emit_const(b, 16);
          int b32 = ny_native_nir_emit_const(b, 32);
          if (bc1 < 0 || bc2 < 0 || bc4 < 0 || bc8 < 0 || bc16 < 0 ||
              b1 < 0 || b2c < 0 || b4c < 0 || b8 < 0 || b16 < 0 || b32 < 0)
            return -1;
          int bm63 = ny_native_nir_emit_const(b, (int64_t)0x7fffffffffffffffLL);
          int bm62 = ny_native_nir_emit_const(b, (int64_t)0x3fffffffffffffffLL);
          int bm60 = ny_native_nir_emit_const(b, (int64_t)0x0fffffffffffffffLL);
          int bm56 = ny_native_nir_emit_const(b, (int64_t)0x00ffffffffffffffLL);
          int bm48 = ny_native_nir_emit_const(b, (int64_t)0x0000ffffffffffffLL);
          if (bm63 < 0 || bm62 < 0 || bm60 < 0 || bm56 < 0 || bm48 < 0)
            return -1;
          /* SWAR bit-reverse steps. */
          int br;
          br = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SAR_I64, .dst = -1, .a = x, .b = b1});
          if (br < 0) return -1;
          br = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = br, .b = bm63});
          if (br < 0) return -1;
          br = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = br, .b = bc1});
          if (br < 0) return -1;
          int bl = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = x, .b = bc1});
          if (bl < 0) return -1;
          bl = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SHL_I64, .dst = -1, .a = bl, .b = b1});
          if (bl < 0) return -1;
          x = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_OR_I64, .dst = -1, .a = br, .b = bl});
          if (x < 0) return -1;
          br = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SAR_I64, .dst = -1, .a = x, .b = b2c});
          if (br < 0) return -1;
          br = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = br, .b = bm62});
          if (br < 0) return -1;
          br = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = br, .b = bc2});
          if (br < 0) return -1;
          bl = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = x, .b = bc2});
          if (bl < 0) return -1;
          bl = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SHL_I64, .dst = -1, .a = bl, .b = b2c});
          if (bl < 0) return -1;
          x = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_OR_I64, .dst = -1, .a = br, .b = bl});
          if (x < 0) return -1;
          br = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SAR_I64, .dst = -1, .a = x, .b = b4c});
          if (br < 0) return -1;
          br = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = br, .b = bm60});
          if (br < 0) return -1;
          br = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = br, .b = bc4});
          if (br < 0) return -1;
          bl = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = x, .b = bc4});
          if (bl < 0) return -1;
          bl = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SHL_I64, .dst = -1, .a = bl, .b = b4c});
          if (bl < 0) return -1;
          x = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_OR_I64, .dst = -1, .a = br, .b = bl});
          if (x < 0) return -1;
          br = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SAR_I64, .dst = -1, .a = x, .b = b8});
          if (br < 0) return -1;
          br = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = br, .b = bm56});
          if (br < 0) return -1;
          br = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = br, .b = bc8});
          if (br < 0) return -1;
          bl = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = x, .b = bc8});
          if (bl < 0) return -1;
          bl = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SHL_I64, .dst = -1, .a = bl, .b = b8});
          if (bl < 0) return -1;
          x = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_OR_I64, .dst = -1, .a = br, .b = bl});
          if (x < 0) return -1;
          br = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SAR_I64, .dst = -1, .a = x, .b = b16});
          if (br < 0) return -1;
          br = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = br, .b = bm48});
          if (br < 0) return -1;
          br = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = br, .b = bc16});
          if (br < 0) return -1;
          bl = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = x, .b = bc16});
          if (bl < 0) return -1;
          bl = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SHL_I64, .dst = -1, .a = bl, .b = b16});
          if (bl < 0) return -1;
          x = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_OR_I64, .dst = -1, .a = br, .b = bl});
          if (x < 0) return -1;
          /* final 32-bit swap */
          br = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SAR_I64, .dst = -1, .a = x, .b = b32});
          if (br < 0) return -1;
          x = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = x, .b = ny_native_nir_emit_const(b, (int64_t)0x00000000FFFFFFFFLL)});
          if (x < 0) return -1;
          x = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SHL_I64, .dst = -1, .a = x, .b = b32});
          if (x < 0) return -1;
          x = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_OR_I64, .dst = -1, .a = x, .b = br});
          if (x < 0) return -1;
        }
        int zero = ny_native_nir_emit_const(b, 0);
        int one = ny_native_nir_emit_const(b, 1);
        int sixtyfour = ny_native_nir_emit_const(b, 64);
        if (zero < 0 || one < 0 || sixtyfour < 0)
          return -1;
        /* is_zero = (x == 0) */
        int is_zero = nyir_emit(
            &b->nyir, (nyir_inst_t){.op = NYIR_CMP_I64,
                                    .dst = -1,
                                    .a = x,
                                    .b = zero,
                                    .cmp = NYIR_CMP_EQ});
        if (is_zero < 0)
          return -1;
        /* lowest = x & -x  (0 - x for negate) */
        int neg = nyir_emit(&b->nyir, (nyir_inst_t){
                                           .op = NYIR_SUB_I64, .dst = -1, .a = zero, .b = x});
        if (neg < 0)
          return -1;
        int lowest = nyir_emit(&b->nyir, (nyir_inst_t){
                                              .op = NYIR_AND_I64, .dst = -1, .a = x, .b = neg});
        if (lowest < 0)
          return -1;
        int lm1 = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SUB_I64,
                                                       .dst = -1,
                                                       .a = lowest,
                                                       .b = one});
        if (lm1 < 0)
          return -1;
        /* Inline SWAR popcount of lm1 (same as ctpop). */
        int c1 = ny_native_nir_emit_const(b, (int64_t)0x5555555555555555LL);
        int c2 = ny_native_nir_emit_const(b, (int64_t)0x3333333333333333LL);
        int c4 = ny_native_nir_emit_const(b, (int64_t)0x0f0f0f0f0f0f0f0fLL);
        int two = ny_native_nir_emit_const(b, 2);
        int four = ny_native_nir_emit_const(b, 4);
        int eight = ny_native_nir_emit_const(b, 8);
        int sixteen = ny_native_nir_emit_const(b, 16);
        int thirtytwo = ny_native_nir_emit_const(b, 32);
        int mask7f = ny_native_nir_emit_const(b, 0x7f);
        if (c1 < 0 || c2 < 0 || c4 < 0 || two < 0 || four < 0 || eight < 0 ||
            sixteen < 0 || thirtytwo < 0 || mask7f < 0)
          return -1;
        int t = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SAR_I64,
                                                     .dst = -1,
                                                     .a = lm1,
                                                     .b = one});
        if (t < 0)
          return -1;
        t = nyir_emit(&b->nyir, (nyir_inst_t){
                                     .op = NYIR_AND_I64, .dst = -1, .a = t, .b = c1});
        if (t < 0)
          return -1;
        int px = nyir_emit(&b->nyir, (nyir_inst_t){
                                          .op = NYIR_SUB_I64, .dst = -1, .a = lm1, .b = t});
        if (px < 0)
          return -1;
        int a = nyir_emit(&b->nyir, (nyir_inst_t){
                                         .op = NYIR_AND_I64, .dst = -1, .a = px, .b = c2});
        int b2 = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SAR_I64,
                                                      .dst = -1,
                                                      .a = px,
                                                      .b = two});
        if (a < 0 || b2 < 0)
          return -1;
        b2 = nyir_emit(&b->nyir, (nyir_inst_t){
                                      .op = NYIR_AND_I64, .dst = -1, .a = b2, .b = c2});
        if (b2 < 0)
          return -1;
        px = nyir_emit(&b->nyir, (nyir_inst_t){
                                      .op = NYIR_ADD_I64, .dst = -1, .a = a, .b = b2});
        if (px < 0)
          return -1;
        t = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SAR_I64,
                                                 .dst = -1,
                                                 .a = px,
                                                 .b = four});
        if (t < 0)
          return -1;
        px = nyir_emit(&b->nyir, (nyir_inst_t){
                                      .op = NYIR_ADD_I64, .dst = -1, .a = px, .b = t});
        if (px < 0)
          return -1;
        px = nyir_emit(&b->nyir, (nyir_inst_t){
                                      .op = NYIR_AND_I64, .dst = -1, .a = px, .b = c4});
        if (px < 0)
          return -1;
        t = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SAR_I64,
                                                 .dst = -1,
                                                 .a = px,
                                                 .b = eight});
        if (t < 0)
          return -1;
        px = nyir_emit(&b->nyir, (nyir_inst_t){
                                      .op = NYIR_ADD_I64, .dst = -1, .a = px, .b = t});
        if (px < 0)
          return -1;
        t = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SAR_I64,
                                                 .dst = -1,
                                                 .a = px,
                                                 .b = sixteen});
        if (t < 0)
          return -1;
        px = nyir_emit(&b->nyir, (nyir_inst_t){
                                      .op = NYIR_ADD_I64, .dst = -1, .a = px, .b = t});
        if (px < 0)
          return -1;
        t = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SAR_I64,
                                                 .dst = -1,
                                                 .a = px,
                                                 .b = thirtytwo});
        if (t < 0)
          return -1;
        px = nyir_emit(&b->nyir, (nyir_inst_t){
                                      .op = NYIR_ADD_I64, .dst = -1, .a = px, .b = t});
        if (px < 0)
          return -1;
        int pop = nyir_emit(&b->nyir, (nyir_inst_t){
                                           .op = NYIR_AND_I64, .dst = -1, .a = px, .b = mask7f});
        if (pop < 0)
          return -1;
        /* result = is_zero ? 64 : pop  — use select via arithmetic:
         * is_zero * 64 + (1-is_zero)*pop, but we only have binary ops.
         * (is_zero * 64) | ((is_zero ^ 1) * pop) */
        int notz = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_XOR_I64,
                                                        .dst = -1,
                                                        .a = is_zero,
                                                        .b = one});
        if (notz < 0)
          return -1;
        int term0 = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_MUL_I64,
                                                         .dst = -1,
                                                         .a = is_zero,
                                                         .b = sixtyfour});
        int term1 = nyir_emit(&b->nyir, (nyir_inst_t){
                                             .op = NYIR_MUL_I64, .dst = -1, .a = notz, .b = pop});
        if (term0 < 0 || term1 < 0)
          return -1;
        return nyir_emit(&b->nyir, (nyir_inst_t){
                                        .op = NYIR_ADD_I64, .dst = -1, .a = term0, .b = term1});
      }
      if ((iname_len == 8 && memcmp(iname, "umax.i64", 8) == 0) ||
          (iname_len == 8 && memcmp(iname, "umin.i64", 8) == 0)) {
        /* Treat as signed for Nytrix i64 values in native path (same select). */
        if (e->as.call.args.len != 3 || e->as.call.args.data[1].name ||
            e->as.call.args.data[2].name) {
          ny_native_nir_fail(b, "native NYIR lower: umax/umin.i64 expects two values");
          return -1;
        }
        bool is_max = iname[1] == 'm' && iname[2] == 'a';
        int x = ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
        int y = ny_native_nir_lower_expr(b, e->as.call.args.data[2].val);
        if (x < 0 || y < 0)
          return -1;
        int c = nyir_emit(&b->nyir, (nyir_inst_t){
                                        .op = NYIR_CMP_I64,
                                        .dst = -1,
                                        .a = x,
                                        .b = y,
                                        .cmp = is_max ? NYIR_CMP_GT : NYIR_CMP_LT});
        int one = ny_native_nir_emit_const(b, 1);
        if (c < 0 || one < 0)
          return -1;
        int nc = nyir_emit(&b->nyir, (nyir_inst_t){
                                          .op = NYIR_XOR_I64, .dst = -1, .a = c, .b = one});
        if (nc < 0)
          return -1;
        int t0 = nyir_emit(&b->nyir, (nyir_inst_t){
                                          .op = NYIR_MUL_I64, .dst = -1, .a = c, .b = x});
        int t1 = nyir_emit(&b->nyir, (nyir_inst_t){
                                          .op = NYIR_MUL_I64, .dst = -1, .a = nc, .b = y});
        if (t0 < 0 || t1 < 0)
          return -1;
        return nyir_emit(&b->nyir, (nyir_inst_t){
                                        .op = NYIR_ADD_I64, .dst = -1, .a = t0, .b = t1});
      }
      if ((iname_len == 8 && memcmp(iname, "smax.i64", 8) == 0) ||
          (iname_len == 8 && memcmp(iname, "smin.i64", 8) == 0)) {
        if (e->as.call.args.len != 3 || e->as.call.args.data[1].name ||
            e->as.call.args.data[2].name) {
          ny_native_nir_fail(b, "native NYIR lower: smax/smin.i64 expects two values");
          return -1;
        }
        bool is_max = iname_len >= 4 && iname[1] == 'm' && iname[2] == 'a';
        int x = ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
        int y = ny_native_nir_lower_expr(b, e->as.call.args.data[2].val);
        if (x < 0 || y < 0)
          return -1;
        /* result = (x > y) ? x : y  via arithmetic select:
         * c = (x > y); c*x + (1-c)*y */
        int c = nyir_emit(&b->nyir, (nyir_inst_t){
                                        .op = NYIR_CMP_I64,
                                        .dst = -1,
                                        .a = x,
                                        .b = y,
                                        .cmp = is_max ? NYIR_CMP_GT : NYIR_CMP_LT});
        int one = ny_native_nir_emit_const(b, 1);
        if (c < 0 || one < 0)
          return -1;
        int nc = nyir_emit(&b->nyir, (nyir_inst_t){
                                          .op = NYIR_XOR_I64, .dst = -1, .a = c, .b = one});
        if (nc < 0)
          return -1;
        int t0 = nyir_emit(&b->nyir, (nyir_inst_t){
                                          .op = NYIR_MUL_I64, .dst = -1, .a = c, .b = x});
        int t1 = nyir_emit(&b->nyir, (nyir_inst_t){
                                          .op = NYIR_MUL_I64, .dst = -1, .a = nc, .b = y});
        if (t0 < 0 || t1 < 0)
          return -1;
        return nyir_emit(&b->nyir, (nyir_inst_t){
                                        .op = NYIR_ADD_I64, .dst = -1, .a = t0, .b = t1});
      }
      if (iname_len == 14 && memcmp(iname, "bitreverse.i64", 14) == 0) {
        if (e->as.call.args.len != 2 || e->as.call.args.data[1].name) {
          ny_native_nir_fail(b,
                             "native NYIR lower: bitreverse.i64 expects one value");
          return -1;
        }
        int x = ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
        if (x < 0)
          return -1;
        /* Portable bit reverse via parallel SWAR. */
        int c1 = ny_native_nir_emit_const(b, (int64_t)0x5555555555555555LL);
        int c2 = ny_native_nir_emit_const(b, (int64_t)0x3333333333333333LL);
        int c4 = ny_native_nir_emit_const(b, (int64_t)0x0f0f0f0f0f0f0f0fLL);
        int c8 = ny_native_nir_emit_const(b, (int64_t)0x00ff00ff00ff00ffLL);
        int c16 = ny_native_nir_emit_const(b, (int64_t)0x0000ffff0000ffffLL);
        int one = ny_native_nir_emit_const(b, 1);
        int two = ny_native_nir_emit_const(b, 2);
        int four = ny_native_nir_emit_const(b, 4);
        int eight = ny_native_nir_emit_const(b, 8);
        int sixteen = ny_native_nir_emit_const(b, 16);
        int thirtytwo = ny_native_nir_emit_const(b, 32);
        if (c1 < 0 || c2 < 0 || c4 < 0 || c8 < 0 || c16 < 0 || one < 0 ||
            two < 0 || four < 0 || eight < 0 || sixteen < 0 || thirtytwo < 0)
          return -1;
        /* x = ((x >> 1) & c1) | ((x & c1) << 1) etc. Use SAR+mask for >> */
        int m63 = ny_native_nir_emit_const(b, (int64_t)0x7fffffffffffffffLL);
        int m62 = ny_native_nir_emit_const(b, (int64_t)0x3fffffffffffffffLL);
        int m60 = ny_native_nir_emit_const(b, (int64_t)0x0fffffffffffffffLL);
        int m56 = ny_native_nir_emit_const(b, (int64_t)0x00ffffffffffffffLL);
        int m48 = ny_native_nir_emit_const(b, (int64_t)0x0000ffffffffffffLL);
        int m32 = ny_native_nir_emit_const(b, (int64_t)0x00000000ffffffffLL);
        if (m63 < 0 || m62 < 0 || m60 < 0 || m56 < 0 || m48 < 0 || m32 < 0)
          return -1;
#define NY_BREV_STEP(xin, sh, msk, cm)                                         \
  do {                                                                         \
    int _r = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SAR_I64,        \
                                                  .dst = -1,                   \
                                                  .a = (xin),                  \
                                                  .b = (sh)});                 \
    if (_r < 0)                                                                \
      return -1;                                                               \
    _r = nyir_emit(&b->nyir, (nyir_inst_t){                                 \
                                  .op = NYIR_AND_I64, .dst = -1, .a = _r,    \
                                  .b = (msk)});                                \
    if (_r < 0)                                                                \
      return -1;                                                               \
    _r = nyir_emit(&b->nyir, (nyir_inst_t){                                 \
                                  .op = NYIR_AND_I64, .dst = -1, .a = _r,    \
                                  .b = (cm)});                                 \
    if (_r < 0)                                                                \
      return -1;                                                               \
    int _l = nyir_emit(&b->nyir, (nyir_inst_t){                             \
                                      .op = NYIR_AND_I64,                    \
                                      .dst = -1,                               \
                                      .a = (xin),                              \
                                      .b = (cm)});                             \
    if (_l < 0)                                                                \
      return -1;                                                               \
    _l = nyir_emit(&b->nyir, (nyir_inst_t){                                 \
                                  .op = NYIR_SHL_I64, .dst = -1, .a = _l,    \
                                  .b = (sh)});                                 \
    if (_l < 0)                                                                \
      return -1;                                                               \
    (xin) = nyir_emit(&b->nyir, (nyir_inst_t){                              \
                                     .op = NYIR_OR_I64, .dst = -1, .a = _r,  \
                                     .b = _l});                                \
    if ((xin) < 0)                                                             \
      return -1;                                                               \
  } while (0)
        NY_BREV_STEP(x, one, m63, c1);
        NY_BREV_STEP(x, two, m62, c2);
        NY_BREV_STEP(x, four, m60, c4);
        NY_BREV_STEP(x, eight, m56, c8);
        NY_BREV_STEP(x, sixteen, m48, c16);
        /* final 32-bit swap */
        {
          int r = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SAR_I64,
                                                       .dst = -1,
                                                       .a = x,
                                                       .b = thirtytwo});
          if (r < 0)
            return -1;
          r = nyir_emit(&b->nyir, (nyir_inst_t){
                                       .op = NYIR_AND_I64, .dst = -1, .a = r, .b = m32});
          if (r < 0)
            return -1;
          int l = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SHL_I64,
                                                       .dst = -1,
                                                       .a = x,
                                                       .b = thirtytwo});
          if (l < 0)
            return -1;
          x = nyir_emit(&b->nyir, (nyir_inst_t){
                                       .op = NYIR_OR_I64, .dst = -1, .a = r, .b = l});
          if (x < 0)
            return -1;
        }
#undef NY_BREV_STEP
        return x;
      }
      if (iname_len == 7 && memcmp(iname, "abs.i64", 7) == 0) {
        if (e->as.call.args.len != 2 || e->as.call.args.data[1].name) {
          ny_native_nir_fail(b, "native NYIR lower: abs.i64 expects one value");
          return -1;
        }
        int x = ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
        if (x < 0)
          return -1;
        int zero = ny_native_nir_emit_const(b, 0);
        int sixtythree = ny_native_nir_emit_const(b, 63);
        if (zero < 0 || sixtythree < 0)
          return -1;
        /* abs via (x ^ (x>>63)) - (x>>63) arithmetic. */
        int s = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SAR_I64,
                                                     .dst = -1,
                                                     .a = x,
                                                     .b = sixtythree});
        if (s < 0)
          return -1;
        int y = nyir_emit(&b->nyir, (nyir_inst_t){
                                         .op = NYIR_XOR_I64, .dst = -1, .a = x, .b = s});
        if (y < 0)
          return -1;
        return nyir_emit(&b->nyir, (nyir_inst_t){
                                        .op = NYIR_SUB_I64, .dst = -1, .a = y, .b = s});
      }
      if (iname_len == 9 && memcmp(iname, "bswap.i64", 9) == 0) {
        if (e->as.call.args.len != 2 || e->as.call.args.data[1].name) {
          ny_native_nir_fail(b, "native NYIR lower: bswap.i64 expects one value");
          return -1;
        }
        int x = ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
        if (x < 0)
          return -1;
        /* Portable byte swap via shifts and masks (no host asm). */
        int c8 = ny_native_nir_emit_const(b, 8);
        int c16 = ny_native_nir_emit_const(b, 16);
        int c24 = ny_native_nir_emit_const(b, 24);
        int c32 = ny_native_nir_emit_const(b, 32);
        int c40 = ny_native_nir_emit_const(b, 40);
        int c48 = ny_native_nir_emit_const(b, 48);
        int c56 = ny_native_nir_emit_const(b, 56);
        int mff = ny_native_nir_emit_const(b, (int64_t)0xff);
        if (c8 < 0 || c16 < 0 || c24 < 0 || c32 < 0 || c40 < 0 || c48 < 0 ||
            c56 < 0 || mff < 0)
          return -1;
        int acc = -1;
        int shifts[] = {c56, c48, c40, c32, c24, c16, c8, -1};
        int rshifts[] = {0, 8, 16, 24, 32, 40, 48, 56};
        for (int bi = 0; bi < 8; ++bi) {
          int piece = x;
          if (rshifts[bi] > 0) {
            int rs = ny_native_nir_emit_const(b, rshifts[bi]);
            if (rs < 0)
              return -1;
            piece = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SAR_I64,
                                                         .dst = -1,
                                                         .a = x,
                                                         .b = rs});
            if (piece < 0)
              return -1;
            /* Logical mask after SAR for high bytes */
            if (rshifts[bi] >= 32) {
              int m = ny_native_nir_emit_const(
                  b, (int64_t)((1ULL << (64 - rshifts[bi])) - 1ULL));
              if (m < 0)
                return -1;
              piece = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_AND_I64,
                                                           .dst = -1,
                                                           .a = piece,
                                                           .b = m});
              if (piece < 0)
                return -1;
            }
          }
          piece = nyir_emit(&b->nyir, (nyir_inst_t){
                                           .op = NYIR_AND_I64, .dst = -1, .a = piece, .b = mff});
          if (piece < 0)
            return -1;
          if (shifts[bi] >= 0) {
            piece = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SHL_I64,
                                                         .dst = -1,
                                                         .a = piece,
                                                         .b = shifts[bi]});
            if (piece < 0)
              return -1;
          }
          if (acc < 0)
            acc = piece;
          else {
            acc = nyir_emit(&b->nyir, (nyir_inst_t){
                                           .op = NYIR_OR_I64, .dst = -1, .a = acc, .b = piece});
            if (acc < 0)
              return -1;
          }
        }
        return acc;
      }
      ny_native_nir_fail(
          b,
          "native NYIR lower: intrinsic(\"%.*s\") is not supported by the Nytrix-owned native backend; use ordinary Nytrix operations or std.math.bin",
          (int)iname_len, iname);
      return -1;
    }
    /* NYIR integer values are raw i64s.  Keep typed print on the raw-i64
     * runtime entry point; rt_print_value is a dynamic NyValue ABI and must
     * only be used after an explicit box operation exists. */
    if (leaf && strcmp(leaf, "print") == 0 &&
        !ny_native_nir_find_user_function(b, name)) {
      if (e->as.call.args.len == 0) {
        ny_native_nir_fail(
            b, "native NYIR lower: print requires at least one positional argument");
        return -1;
      }
      for (size_t i = 0; i < e->as.call.args.len; ++i) {
        if (e->as.call.args.data[i].name) {
          ny_native_nir_fail(b, "native NYIR lower: print accepts positional arguments only");
          return -1;
        }
        const expr_t *arg = e->as.call.args.data[i].val;
        if (ny_native_nir_expr_is_f64(b, arg) || ny_native_nir_expr_is_f32(b, arg)) {
          ny_native_nir_fail(
              b, "native NYIR lower: print currently supports integer/string arguments only");
          return -1;
        }
        int raw = ny_native_nir_lower_expr(b, arg);
        const char *print_sym = arg && arg->kind == NY_E_LITERAL &&
                                        arg->as.literal.kind == NY_LIT_STR
                                    ? "rt_print_cstr" : "rt_print_i64_raw";
        if (raw < 0 || ny_native_nir_emit_runtime_call(
                           b, print_sym, raw, -1, -1, 1, 0) < 0)
          return -1;
      }
      if (nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_CALL,
                                               .dst = -1,
                                               .a = -1,
                                               .b = -1,
                                               .c = -1,
                                               .imm = 0,
                                               .flags = NYIR_INST_F_EXTERN,
                                               .symbol = "rt_print_newline"}) < 0) {
        ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
        return -1;
      }
      return ny_native_nir_emit_const(b, 0);
    }
    if (leaf && strcmp(leaf, "float") == 0) {
      if (e->as.call.args.len != 1 || e->as.call.args.data[0].name) {
        ny_native_nir_fail(b, "native NYIR lower: float requires one positional argument");
        return -1;
      }
      const expr_t *arg = e->as.call.args.data[0].val;
      int value = ny_native_nir_lower_expr(b, arg);
      if (value < 0)
        return -1;
      return ny_native_nir_expr_is_f64(b, arg) ? value :
          ny_native_nir_emit_i64_to_f64(b, value);
    }
    if (leaf && strcmp(leaf, "ticks") == 0) {
      if (e->as.call.args.len != 0) {
        ny_native_nir_fail(b, "native NYIR lower: ticks takes no arguments");
        return -1;
      }
      return ny_native_nir_emit_runtime_call(b, "rt_ticks_ns", -1, -1, -1,
                                             0, 0);
    }
    if (leaf && strcmp(leaf, "__flt_sqrt") == 0) {
      if (e->as.call.args.len != 1 || e->as.call.args.data[0].name) {
        ny_native_nir_fail(b, "native NYIR lower: __flt_sqrt requires one positional argument");
        return -1;
      }
      const expr_t *arg = e->as.call.args.data[0].val;
      int value = ny_native_nir_lower_expr(b, arg);
      if (value < 0)
        return -1;
      if (!ny_native_nir_expr_is_f64(b, arg)) {
        value = ny_native_nir_emit_i64_to_f64(b, value);
        if (value < 0)
          return -1;
      }
      return ny_native_nir_emit_runtime_call(b, "rt_native_sqrt_f64", value,
                                             -1, -1, 1, NYIR_INST_F_RET_F64);
    }
    if (leaf && (strcmp(leaf, "addr_of") == 0 || strcmp(leaf, "borrow") == 0)) {
      if (e->as.call.args.len != 1 || e->as.call.args.data[0].name ||
          !e->as.call.args.data[0].val) {
        ny_native_nir_fail(
            b, "native NYIR lower: %s requires one addressable expression",
            leaf);
        return -1;
      }
      const expr_t *target = e->as.call.args.data[0].val;
      if (target->kind == NY_E_DEREF)
        return ny_native_nir_lower_expr(b, target->as.deref.target);
      if (target->kind != NY_E_IDENT) {
        ny_native_nir_fail(
            b, "native NYIR lower: %s supports local and dereferenced pointer lvalues, not expression kind %d",
            leaf, (int)target->kind);
        return -1;
      }
      const char *local_name = target->as.ident.name;
      ny_native_nir_local_t *l = ny_native_nir_find_local(b, local_name);
      if (!l) {
        int v = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_ADDR_SYMBOL,
                                                     .dst = -1,
                                                     .a = -1,
                                                     .b = -1,
                                                     .imm = 0,
                                                     .symbol = local_name});
        if (v < 0)
          ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
        return v;
      }
      return ny_native_nir_emit_addr_local(b, l->slot, local_name);
    }
    if (leaf && strcmp(leaf, "f64buf_new") == 0) {
      if (e->as.call.args.len != 1 || e->as.call.args.data[0].name) {
        ny_native_nir_fail(b, "native NYIR lower: f64buf_new requires one positional length");
        return -1;
      }
      int count = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
      int width = ny_native_nir_emit_const(b, 8);
      return count < 0 || width < 0 ? -1 :
          ny_native_nir_emit_runtime_call(b, "rt_native_tbuf_new", count, width,
                                          -1, 2, 0);
    }
    if (leaf && strcmp(leaf, "f64buf_load") == 0) {
      if (e->as.call.args.len != 2 || e->as.call.args.data[0].name ||
          e->as.call.args.data[1].name) {
        ny_native_nir_fail(b, "native NYIR lower: f64buf_load requires buffer and index");
        return -1;
      }
      int data = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
      int index = ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
      int width = ny_native_nir_emit_const(b, 8);
      int offset = width < 0 ? -1 : ny_native_nir_push_val(b, NYIR_MUL_I64,
                                                              index, width, 0, NULL);
      int addr = offset < 0 ? -1 : ny_native_nir_emit_add_i64(b, data, offset);
      return data < 0 || index < 0 || addr < 0 ? -1 :
          ny_native_nir_emit_load_f64(b, addr);
    }
    if (leaf && strcmp(leaf, "f64buf_store") == 0) {
      if (e->as.call.args.len != 3 || e->as.call.args.data[0].name ||
          e->as.call.args.data[1].name || e->as.call.args.data[2].name) {
        ny_native_nir_fail(b, "native NYIR lower: f64buf_store requires buffer, index, and value");
        return -1;
      }
      int data = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
      int index = ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
      int value = ny_native_nir_lower_expr(b, e->as.call.args.data[2].val);
      int width = ny_native_nir_emit_const(b, 8);
      int offset = width < 0 ? -1 : ny_native_nir_push_val(b, NYIR_MUL_I64,
                                                              index, width, 0, NULL);
      int addr = offset < 0 ? -1 : ny_native_nir_emit_add_i64(b, data, offset);
      return data < 0 || index < 0 || value < 0 || addr < 0 ? -1 :
          (ny_native_nir_emit_store_f64(b, addr, value) ? value : -1);
    }
    if (leaf && (strcmp(leaf, "load64_i") == 0 ||
                 strcmp(leaf, "load64_h") == 0 ||
                 strcmp(leaf, "__load64_h") == 0 ||
                 strcmp(leaf, "__load64_idx") == 0)) {
      if (e->as.call.args.len < 1 || e->as.call.args.len > 2 ||
          e->as.call.args.data[0].name ||
          (e->as.call.args.len > 1 && e->as.call.args.data[1].name)) {
        ny_native_nir_fail(b, "native NYIR lower: load64_i/load64_h require positional pointer and optional offset");
        return -1;
      }
      int addr = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
      if (addr < 0)
        return -1;
      if (e->as.call.args.len > 1) {
        int off = ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
        if (off < 0)
          return -1;
        addr = ny_native_nir_emit_add_i64(b, addr, off);
        if (addr < 0)
          return -1;
      }
      return ny_native_nir_emit_load_i64(b, addr);
    }
    if (leaf && (strcmp(leaf, "store64_i") == 0 ||
                 strcmp(leaf, "store64_h") == 0 ||
                 strcmp(leaf, "__store64_h") == 0 ||
                 strcmp(leaf, "__store64_idx") == 0)) {
      bool intrinsic_order = strcmp(leaf, "__store64_h") == 0 ||
                             strcmp(leaf, "__store64_idx") == 0;
      if (e->as.call.args.len < 2 || e->as.call.args.len > 3 ||
          (intrinsic_order && e->as.call.args.len != 3) ||
          e->as.call.args.data[0].name || e->as.call.args.data[1].name ||
          (e->as.call.args.len > 2 && e->as.call.args.data[2].name)) {
        ny_native_nir_fail(b, "native NYIR lower: store64_i/store64_h require positional pointer, value, and optional offset");
        return -1;
      }
      size_t val_idx = intrinsic_order ? 2u : 1u;
      size_t off_idx = intrinsic_order ? 1u : 2u;
      int addr = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
      int value = ny_native_nir_lower_expr(b, e->as.call.args.data[val_idx].val);
      if (addr < 0 || value < 0)
        return -1;
      if (e->as.call.args.len > off_idx) {
        int off = ny_native_nir_lower_expr(b, e->as.call.args.data[off_idx].val);
        if (off < 0)
          return -1;
        addr = ny_native_nir_emit_add_i64(b, addr, off);
        if (addr < 0)
          return -1;
      }
      if (!ny_native_nir_emit_store_i64(b, addr, value))
        return -1;
      return ny_native_nir_emit_const(b, 0);
    }
    if (e->as.call.args.len > NYIR_CALL_MAX_ARGS) {
      ny_native_nir_fail(b,
                         "native NYIR lower: call exceeds the maximum supported argument count (%d)",
                         NYIR_CALL_MAX_ARGS);
      return -1;
    }
    const stmt_t *callee_fn = ny_native_nir_find_user_function(b, name);
    int args[NYIR_CALL_MAX_ARGS];
    for (size_t i = 0; i < e->as.call.args.len; ++i) {
      if (e->as.call.args.data[i].name) {
        ny_native_nir_fail(b, "native NYIR lower: named call args are not supported");
        return -1;
      }
      bool arg_expr_f64 = ny_native_nir_expr_is_f64(b, e->as.call.args.data[i].val);
      bool arg_expr_f32 = ny_native_nir_expr_is_f32(b, e->as.call.args.data[i].val);
      args[i] = ny_native_nir_lower_expr(b, e->as.call.args.data[i].val);
      if (args[i] < 0)
        return -1;
      if (callee_fn && i < callee_fn->as.fn.params.len) {
        const char *param_type = callee_fn->as.fn.params.data[i].type;
        if (ny_native_type_name_is_f32(param_type) && !arg_expr_f32) {
          args[i] = arg_expr_f64 ? ny_native_nir_emit_f64_to_f32(b, args[i])
                                 : ny_native_nir_emit_i64_to_f32(b, args[i]);
          if (args[i] < 0)
            return -1;
        } else if (ny_native_type_name_is_f64(param_type) && !arg_expr_f64) {
          args[i] = arg_expr_f32 ? ny_native_nir_emit_f32_to_f64(b, args[i])
                                 : ny_native_nir_emit_i64_to_f64(b, args[i]);
          if (args[i] < 0)
            return -1;
        }
      }
    }
    const ny_extern_entry_t *ext =
        b->externs ? ny_extern_table_lookup(b->externs, name) : NULL;
    bool has_aggregate_return = ext && ext->ret_aggregate_size > 0;
    bool has_sret = has_aggregate_return &&
                    ext->ret_aggregate_classes[0] == NY_SYSV_AGG_MEMORY;
    if (has_aggregate_return && !has_sret &&
        ext->ret_aggregate_classes[0] != NY_SYSV_AGG_INTEGER &&
        ext->ret_aggregate_classes[0] != NY_SYSV_AGG_SSE &&
        ext->ret_aggregate_classes[0] != NY_SYSV_AGG_HFA_F32 &&
        ext->ret_aggregate_classes[0] != NY_SYSV_AGG_HFA_F64 &&
        ext->ret_aggregate_classes[0] != NY_SYSV_AGG_HVA_V128) {
      ny_native_nir_fail(
          b, "native NYIR lower: aggregate return class is not represented for the selected ABI");
      return -1;
    }
    if (ext) {
      for (unsigned i = 0;
           i < ext->param_count && i < e->as.call.args.len; ++i) {
        if (ext->arg_aggregate_sizes[i] > 0 &&
            NYIR_ARG_AGG_SIZE(ext->arg_aggregate_sizes[i]) <= 16 &&
            (NYIR_ARG_AGG_CLASS(ext->arg_aggregate_sizes[i], 0) ==
                 NY_SYSV_AGG_UNSUPPORTED ||
             NYIR_ARG_AGG_CLASS(ext->arg_aggregate_sizes[i], 0) ==
                 NY_SYSV_AGG_NONE)) {
          ny_native_nir_fail(
              b, "native NYIR lower: register aggregate argument is not represented for the selected ABI");
          return -1;
        }
      }
    }
    int aggregate_ret_ptr = -1;
    if (has_aggregate_return) {
      aggregate_ret_ptr = nyir_emit(
          &b->nyir,
          (nyir_inst_t){.op = NYIR_ALLOCA,
                          .dst = -1,
                          .a = -1,
                          .b = -1,
                          .c = -1,
                          .imm = ext->ret_aggregate_size});
      if (aggregate_ret_ptr < 0) {
        ny_native_nir_fail(b, "native NYIR lower: aggregate return allocation failed");
        return -1;
      }
    }

    uint32_t *arg_sizes = NULL;
    if (ext && ext->param_count > 0) {
      bool has_byval = false;
      for (unsigned i = 0; i < ext->param_count; ++i) {
        if (ext->arg_aggregate_sizes[i] > 0) has_byval = true;
      }
      if (has_byval) {
        size_t total_args = e->as.call.args.len + (has_sret ? 1 : 0);
        arg_sizes = (uint32_t *)calloc(total_args, sizeof(*arg_sizes));
        if (!arg_sizes) {
          ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
          return -1;
        }
        for (unsigned i = 0;
             i < e->as.call.args.len && i < ext->param_count; ++i) {
          arg_sizes[i + (has_sret ? 1 : 0)] = ext->arg_aggregate_sizes[i];
        }
      }
    }

    size_t original_argc = e->as.call.args.len;
    size_t argc = original_argc + (has_sret ? 1 : 0);
    if (has_sret) {
      if (argc > NYIR_CALL_MAX_ARGS) {
        free(arg_sizes);
        ny_native_nir_fail(b, "native NYIR lower: call exceeds maximum args with sret");
        return -1;
      }
      for (int i = (int)original_argc - 1; i >= 0; --i) {
        args[i + 1] = args[i];
      }
      args[0] = aggregate_ret_ptr;
    }

    bool builtin_c_call = leaf && (strcmp(leaf, "malloc") == 0 ||
                                   strcmp(leaf, "__malloc") == 0 ||
                                   strcmp(leaf, "realloc") == 0 ||
                                   strcmp(leaf, "__realloc") == 0 ||
                                   strcmp(leaf, "free") == 0 ||
                                   strcmp(leaf, "__free") == 0);
    const char *symbol = ext ? ext->c_symbol :
                         builtin_c_call && strstr(leaf, "realloc") ? "realloc" :
                         builtin_c_call && strstr(leaf, "malloc") ? "malloc" :
                         builtin_c_call ? "free" : name;
    unsigned flags = (ext || builtin_c_call) ? NYIR_INST_F_EXTERN : 0;
    if (has_sret)
      flags |= NYIR_INST_F_SRET;
    if (callee_fn && ny_native_type_name_is_f64(callee_fn->as.fn.return_type)) {
      flags |= NYIR_INST_F_RET_F64;
    } else if (callee_fn && ny_native_type_name_is_f32(callee_fn->as.fn.return_type)) {
      flags |= NYIR_INST_F_RET_F32;
    } else if (ny_native_nir_expr_is_f64(b, e)) {
      flags |= NYIR_INST_F_RET_F64;
    }
    if (has_aggregate_return && !has_sret) {
      if (ext->ret_aggregate_classes[0] == NY_SYSV_AGG_SSE ||
          ext->ret_aggregate_classes[0] == NY_SYSV_AGG_HFA_F64)
        flags |= NYIR_INST_F_RET_F64;
      else if (ext->ret_aggregate_classes[0] == NY_SYSV_AGG_HFA_F32)
        flags |= NYIR_INST_F_RET_F32;
    }
    int *extra = NULL;
    if (argc > 6) {
      size_t extra_len = argc - 6;
      extra = (int *)malloc(extra_len * sizeof(*extra));
      if (!extra) {
        free(arg_sizes);
        ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
        return -1;
      }
      memcpy(extra, &args[6], extra_len * sizeof(*extra));
    }
    int v = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_CALL,
                                                 .dst = -1,
                                                 .a = argc > 0 ? args[0] : -1,
                                                 .b = argc > 1 ? args[1] : -1,
                                                 .c = argc > 2 ? args[2] : -1,
                                                 .d = argc > 3 ? args[3] : -1,
                                                 .e = argc > 4 ? args[4] : -1,
                                                 .f = argc > 5 ? args[5] : -1,
                                                 .imm = (int64_t)argc,
                                                 .flags = flags,
                                                 .symbol = symbol,
                                                 .extra_args = extra,
                                                 .extra_args_len = argc > 6 ? argc - 6 : 0,
                                                 .arg_sizes = arg_sizes});
    if (v < 0) {
      free(extra);
      free(arg_sizes);
      ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
      return -1;
    }
    if (has_aggregate_return && !has_sret &&
        ext->ret_aggregate_classes[0] == NY_SYSV_AGG_HVA_V128) {
      unsigned elem_count = ext->ret_aggregate_size / 16u;
      if (elem_count < 1 || elem_count > 4 ||
          elem_count * 16u != ext->ret_aggregate_size) {
        ny_native_nir_fail(b, "native NYIR lower: invalid AAPCS64 HVA return layout");
        return -1;
      }
      for (unsigned i = 0; i < elem_count; ++i) {
        int captured = nyir_emit(
            &b->nyir, (nyir_inst_t){.op = NYIR_CAPTURE_RET,
                                     .dst = -1, .a = -1, .b = -1, .c = -1,
                                     .imm = 10 + (int64_t)i});
        int off = i ? ny_native_nir_emit_const(b, (int64_t)i * 16) : -1;
        int addr = i ? ny_native_nir_emit_add_i64(b, aggregate_ret_ptr, off)
                     : aggregate_ret_ptr;
        if (captured < 0 || addr < 0 ||
            nyir_emit(&b->nyir,
                        (nyir_inst_t){.op = NYIR_VEC4_STORE_I64,
                                        .dst = -1, .a = addr, .b = captured,
                                        .c = -1}) < 0)
          return -1;
      }
      return aggregate_ret_ptr;
    }
    if (has_aggregate_return && !has_sret &&
        (ext->ret_aggregate_classes[0] == NY_SYSV_AGG_HFA_F32 ||
         ext->ret_aggregate_classes[0] == NY_SYSV_AGG_HFA_F64)) {
      bool f32_hfa = ext->ret_aggregate_classes[0] == NY_SYSV_AGG_HFA_F32;
      unsigned elem_size = f32_hfa ? 4u : 8u;
      unsigned elem_count = ext->ret_aggregate_size / elem_size;
      if (elem_count < 1 || elem_count > 4 ||
          elem_count * elem_size != ext->ret_aggregate_size) {
        ny_native_nir_fail(b, "native NYIR lower: invalid AAPCS64 HFA return layout");
        return -1;
      }
      static const int f64_selectors[4] = {2, 3, 8, 9};
      int captured[4] = {-1, -1, -1, -1};
      for (unsigned i = 0; i < elem_count; ++i) {
        captured[i] = nyir_emit(
            &b->nyir, (nyir_inst_t){.op = NYIR_CAPTURE_RET,
                                     .dst = -1, .a = -1, .b = -1, .c = -1,
                                     .imm = f32_hfa ? 4 + (int64_t)i
                                                    : f64_selectors[i]});
        if (captured[i] < 0)
          return -1;
      }
      if (!f32_hfa) {
        for (unsigned i = 0; i < elem_count; ++i) {
          int off = i ? ny_native_nir_emit_const(b, (int64_t)i * 8) : -1;
          int addr = i ? ny_native_nir_emit_add_i64(b, aggregate_ret_ptr, off)
                       : aggregate_ret_ptr;
          if (addr < 0 ||
              !ny_native_nir_emit_store_i64(b, addr, captured[i]))
            return -1;
        }
      } else {
        int shift32 = ny_native_nir_emit_const(b, 32);
        if (shift32 < 0)
          return -1;
        for (unsigned pair = 0; pair * 2 < elem_count; ++pair) {
          unsigned lo_idx = pair * 2;
          int packed = captured[lo_idx];
          if (lo_idx + 1 < elem_count) {
            int shifted = nyir_emit(
                &b->nyir, (nyir_inst_t){.op = NYIR_SHL_I64,
                                         .dst = -1,
                                         .a = captured[lo_idx + 1],
                                         .b = shift32});
            packed = shifted < 0 ? -1 : nyir_emit(
                &b->nyir, (nyir_inst_t){.op = NYIR_OR_I64,
                                         .dst = -1, .a = captured[lo_idx],
                                         .b = shifted});
          }
          int off = pair ? ny_native_nir_emit_const(b, (int64_t)pair * 8) : -1;
          int addr = pair ? ny_native_nir_emit_add_i64(b, aggregate_ret_ptr, off)
                          : aggregate_ret_ptr;
          if (packed < 0 || addr < 0 ||
              !ny_native_nir_emit_store_i64(b, addr, packed))
            return -1;
        }
      }
      return aggregate_ret_ptr;
    }
    int primary_ret = v;
    int second_ret = -1;
    bool capture_second_integer_first =
        has_aggregate_return && !has_sret &&
        ext->ret_aggregate_classes[0] == NY_SYSV_AGG_SSE &&
        ext->ret_aggregate_classes[1] == NY_SYSV_AGG_INTEGER;
    if (capture_second_integer_first) {
      second_ret = nyir_emit(
          &b->nyir, (nyir_inst_t){.op = NYIR_CAPTURE_RET,
                                   .dst = -1,
                                   .a = -1,
                                   .b = -1,
                                   .c = -1,
                                   .imm = 1});
      if (second_ret < 0) {
        ny_native_nir_fail(b, "native NYIR lower: secondary return register capture failed");
        return -1;
      }
    }
    if (has_aggregate_return && !has_sret &&
        ext->ret_aggregate_classes[0] == NY_SYSV_AGG_SSE) {
      primary_ret = nyir_emit(
          &b->nyir, (nyir_inst_t){.op = NYIR_CAPTURE_RET,
                                   .dst = -1,
                                   .a = -1,
                                   .b = -1,
                                   .c = -1,
                                   .imm = 2});
      if (primary_ret < 0) {
        ny_native_nir_fail(b, "native NYIR lower: primary return register capture failed");
        return -1;
      }
    }
    if (has_aggregate_return && !has_sret &&
        ext->ret_aggregate_classes[1] != NY_SYSV_AGG_NONE &&
        !capture_second_integer_first) {
      int selector = -1;
      if (ext->ret_aggregate_classes[1] == NY_SYSV_AGG_INTEGER)
        selector = ext->ret_aggregate_classes[0] == NY_SYSV_AGG_INTEGER ? 0 : 1;
      else if (ext->ret_aggregate_classes[1] == NY_SYSV_AGG_SSE)
        selector = ext->ret_aggregate_classes[0] == NY_SYSV_AGG_SSE ? 3 : 2;
      if (selector < 0) {
        ny_native_nir_fail(
            b, "native NYIR lower: secondary aggregate return class is not represented for the selected ABI");
        return -1;
      }
      second_ret = nyir_emit(
          &b->nyir, (nyir_inst_t){.op = NYIR_CAPTURE_RET,
                                   .dst = -1,
                                   .a = -1,
                                   .b = -1,
                                   .c = -1,
                                   .imm = selector});
      if (second_ret < 0) {
        ny_native_nir_fail(b, "native NYIR lower: return register capture failed");
        return -1;
      }
    }
    if (has_aggregate_return && !has_sret) {
      if (!ny_native_nir_emit_store_i64(b, aggregate_ret_ptr, primary_ret))
        return -1;
      if (second_ret >= 0) {
        int off = ny_native_nir_emit_const(b, 8);
        int addr = off >= 0
                       ? ny_native_nir_emit_add_i64(b, aggregate_ret_ptr, off)
                       : -1;
        if (addr < 0 ||
            !ny_native_nir_emit_store_i64(b, addr, second_ret))
          return -1;
      }
    }
    return has_aggregate_return ? aggregate_ret_ptr : v;
  }
  case NY_E_DEREF: {
    int addr = ny_native_nir_lower_expr(b, e->as.deref.target);
    if (addr < 0)
      return -1;
    int v = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_LOAD_I64,
                                                 .dst = -1,
                                                 .a = addr,
                                                 .b = -1,
                                                 .c = -1});
    if (v < 0)
      ny_native_nir_fail(b, "native NYIR lower: deref load failed");
    return v;
  }
  case NY_E_ASM: {
    const char *tpl = e->as.as_asm.code ? e->as.as_asm.code : "";
    const char *cons = e->as.as_asm.constraints ? e->as.as_asm.constraints : "";
    if (e->as.as_asm.args.len == 0) {
      if (!*tpl || strcmp(tpl, "nop") == 0 || strcmp(tpl, "nop;") == 0 ||
          strcmp(tpl, "nop\n") == 0 || strcmp(tpl, "nop;nop") == 0 ||
          strcmp(tpl, "nop;nop;nop") == 0 || strcmp(tpl, "pause") == 0 ||
          strcmp(tpl, "pause;") == 0 || strcmp(tpl, "lfence") == 0 ||
          strcmp(tpl, "mfence") == 0 || strcmp(tpl, "sfence") == 0 ||
          strcmp(tpl, "ud2") == 0 || strcmp(tpl, "yield") == 0 ||
          strcmp(tpl, "wfe") == 0 || strcmp(tpl, "wfi") == 0 ||
          strcmp(tpl, "sev") == 0 || strcmp(tpl, "isb") == 0 ||
          strcmp(tpl, "isb sy") == 0 || strcmp(tpl, "dmb") == 0 ||
          strcmp(tpl, "dmb ish") == 0 || strcmp(tpl, "dmb sy") == 0 ||
          strcmp(tpl, "dsb") == 0 || strcmp(tpl, "dsb ish") == 0 ||
          strcmp(tpl, "dsb sy") == 0 || strcmp(tpl, "xor %eax,%eax") == 0 ||
          strcmp(tpl, "xorl %eax,%eax") == 0 || strcmp(tpl, "xor %%eax,%%eax") == 0 ||
          strcmp(tpl, "xor %rax,%rax") == 0 || strcmp(tpl, "xorq %rax,%rax") == 0)
        return ny_native_nir_emit_const(b, 0);
    }
    if (e->as.as_asm.args.len == 1 && strcmp(cons, "=r,r") == 0 &&
        (strcmp(tpl, "mov $1, $0") == 0 || strcmp(tpl, "mov %1, %0") == 0 ||
         strcmp(tpl, "movq $1, $0") == 0 || strcmp(tpl, "movl $1, $0") == 0 ||
         strcmp(tpl, "mov $1,$0") == 0 || strcmp(tpl, "mov %1,%0") == 0))
      return ny_native_nir_lower_expr(b, e->as.as_asm.args.data[0]);
    if (e->as.as_asm.args.len == 2 &&
        (strcmp(tpl, "addq $1, $0") == 0 || strcmp(tpl, "add %1, %0") == 0 ||
         strcmp(tpl, "or $0, $1, $2") == 0)) {
      int a = ny_native_nir_lower_expr(b, e->as.as_asm.args.data[0]);
      int c = a < 0 ? -1 : ny_native_nir_lower_expr(b, e->as.as_asm.args.data[1]);
      if (a < 0 || c < 0)
        return -1;
      return ny_native_asm_emit_binop(b,
          strncmp(tpl, "or ", 3) == 0 ? NYIR_OR_I64 : NYIR_ADD_I64, a, c);
    }
    return ny_native_nir_lower_aarch64_asm(b, e);
  }
  default:
    ny_native_nir_fail(b, "native NYIR lower: expression kind %d is not in shared NYIR yet",
                       (int)e->kind);
    return -1;
  }
}

static int ny_native_nir_lower_logical(ny_native_nir_builder_t *b,
                                       const expr_t *left,
                                       const expr_t *right,
                                       bool is_or) {
  int result_slot = ny_native_nir_temp_slot(b);
  int zero = ny_native_nir_emit_const(b, 0);
  int one = ny_native_nir_emit_const(b, 1);
  if (zero < 0 || one < 0)
    return -1;
  int true_label = b->next_label++;
  int end_label = b->next_label++;

  if (!ny_native_nir_store_local_value(b, result_slot, zero))
    return -1;

  int lhs = ny_native_nir_lower_expr(b, left);
  if (lhs < 0)
    return -1;

  if (is_or) {
    if (!ny_native_nir_emit_br_if(b, lhs, true_label))
      return -1;
  } else {
    int lhs_zero = ny_native_nir_emit_is_zero(b, lhs);
    if (lhs_zero < 0 || !ny_native_nir_emit_br_if(b, lhs_zero, end_label))
      return -1;
  }

  int rhs = ny_native_nir_lower_expr(b, right);
  if (rhs < 0)
    return -1;

  if (is_or) {
    if (!ny_native_nir_emit_br_if(b, rhs, true_label) ||
        !ny_native_nir_emit_br(b, end_label))
      return -1;
  } else {
    int rhs_zero = ny_native_nir_emit_is_zero(b, rhs);
    if (rhs_zero < 0 || !ny_native_nir_emit_br_if(b, rhs_zero, end_label))
      return -1;
  }

  if (!ny_native_nir_emit_label(b, true_label) ||
      !ny_native_nir_store_local_value(b, result_slot, one) ||
      !ny_native_nir_emit_label(b, end_label))
    return -1;

  return ny_native_nir_load_local_value(b, result_slot);
}

static int ny_native_nir_lower_ternary(ny_native_nir_builder_t *b,
                                       const expr_t *cond,
                                       const expr_t *true_expr,
                                       const expr_t *false_expr) {
  if (!cond || !true_expr || !false_expr) {
    ny_native_nir_fail(b, "native NYIR lower: malformed ternary expression");
    return -1;
  }
  int result_slot = ny_native_nir_temp_slot(b);
  int true_label = b->next_label++;
  int else_label = b->next_label++;
  int end_label = b->next_label++;

  int cond_val = ny_native_nir_lower_expr(b, cond);
  if (cond_val < 0)
    return -1;
  if (!ny_native_nir_emit_br_if(b, cond_val, true_label) ||
      !ny_native_nir_emit_br(b, else_label) ||
      !ny_native_nir_emit_label(b, true_label))
    return -1;

  int true_val = ny_native_nir_lower_expr(b, true_expr);
  if (true_val < 0 ||
      !ny_native_nir_store_local_value(b, result_slot, true_val) ||
      !ny_native_nir_emit_br(b, end_label))
    return -1;

  if (!ny_native_nir_emit_label(b, else_label))
    return -1;
  int false_val = ny_native_nir_lower_expr(b, false_expr);
  if (false_val < 0 ||
      !ny_native_nir_store_local_value(b, result_slot, false_val) ||
      !ny_native_nir_emit_label(b, end_label))
    return -1;

  return ny_native_nir_load_local_value(b, result_slot);
}

static bool ny_native_nir_lower_stmt(ny_native_nir_builder_t *b, const stmt_t *s);

static bool ny_native_nir_lower_var(ny_native_nir_builder_t *b, const stmt_t *s) {
  const stmt_var_t *v = &s->as.var;
  if (v->is_del || v->is_destructure)
    return ny_native_nir_fail(b,
                              "native NYIR lower: only simple def/mut bindings are supported");
  for (size_t i = 0; i < v->names.len; ++i) {
    const char *name = v->names.data[i];
    if (!name || strcmp(name, "_") == 0)
      continue;
    if (i >= v->exprs.len || !v->exprs.data[i])
      return ny_native_nir_fail(b,
                                "native NYIR lower: local '%s' needs an initializer",
                                name);
    bool is_f64 = i < v->types.len && ny_native_type_name_is_f64(v->types.data[i]);
    bool is_f32 = i < v->types.len && ny_native_type_name_is_f32(v->types.data[i]);
    if (!is_f64 && !is_f32 && i < v->exprs.len)
      is_f64 = ny_native_nir_expr_is_f64(b, v->exprs.data[i]);
    if (!is_f64 && !is_f32 && i < v->exprs.len)
      is_f32 = ny_native_nir_expr_is_f32(b, v->exprs.data[i]);
    ny_native_nir_local_t *l =
        v->is_decl ? ny_native_nir_bind_local_typed(b, name, is_f64, is_f32)
                   : ny_native_nir_add_local(b, name);
    if (l && is_f64)
      l->is_f64 = true;
    if (l && is_f32)
      l->is_f32 = true;
    if (!l)
      return false;
    expr_t *init = v->exprs.data[i];
    int val = -1;
    if (is_f32 && init && init->kind == NY_E_LITERAL &&
        init->as.literal.kind == NY_LIT_FLOAT)
      val = ny_native_nir_emit_const_f32(b, init->as.literal.as.f);
    else
      val = ny_native_nir_lower_expr(b, init);
    if (val < 0)
      return false;
    if (is_f64 && init && ny_native_nir_expr_is_f32(b, init)) {
      val = ny_native_nir_emit_f32_to_f64(b, val);
      if (val < 0)
        return false;
    }
    size_t before = b->nyir.len;
    nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_STORE_LOCAL,
                                         .dst = -1,
                                         .a = val,
                                         .b = -1,
                                         .imm = l->slot,
                                         .symbol = l->name});
    if (b->nyir.len == before)
      return ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
    b->last_value = val;
  }
  return true;
}

static bool ny_native_nir_lower_if(ny_native_nir_builder_t *b, const stmt_t *s) {
  if (s->as.iff.init && !ny_native_nir_lower_stmt(b, s->as.iff.init))
    return false;
  int cond = ny_native_nir_lower_expr(b, s->as.iff.test);
  if (cond < 0)
    return false;
  /* Statement-only `if` has no expression result.  Do not synthesize a
   * merge local from the prior statement's value: that can mix unrelated
   * scalar types across the branch (for example f64 work before int code). */
  if (!s->as.iff.alt) {
    int then_label = b->next_label++;
    int end_label = b->next_label++;
    bool entry_return = b->emitted_return;
    int entry_last_value = b->last_value;
    if (!ny_native_nir_emit_br_if(b, cond, then_label) ||
        !ny_native_nir_emit_br(b, end_label) ||
        !ny_native_nir_emit_label(b, then_label))
      return false;
    b->emitted_return = false;
    if (!ny_native_nir_lower_stmt(b, s->as.iff.conseq))
      return false;
    if (!b->emitted_return && !ny_native_nir_emit_br(b, end_label))
      return false;
    if (!ny_native_nir_emit_label(b, end_label))
      return false;
    b->emitted_return = entry_return;
    b->last_value = entry_last_value;
    return true;
  }
  int then_label = b->next_label++;
  int else_label = b->next_label++;
  int merge_label = b->next_label++;
  int end_label = b->next_label++;
  if (!ny_native_nir_emit_br_if(b, cond, then_label) ||
      !ny_native_nir_emit_br(b, else_label) ||
      !ny_native_nir_emit_label(b, then_label))
    return false;

  int result_slot = ny_native_nir_temp_slot(b);
  bool has_alt = s->as.iff.alt != NULL;
  bool entry_return = b->emitted_return;
  int entry_last_value = b->last_value;

  /* If no else, pre-store entry_last_value as the false-branch result. */
  if (!has_alt && !entry_return && entry_last_value >= 0) {
    size_t before = b->nyir.len;
    nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_STORE_LOCAL,
                                         .dst = -1,
                                         .a = entry_last_value,
                                         .b = -1,
                                         .imm = result_slot});
    if (b->nyir.len == before)
      return ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
  }

  /* Then branch. */
  b->emitted_return = false;
  if (!ny_native_nir_lower_stmt(b, s->as.iff.conseq))
    return false;
  bool conseq_returns = b->emitted_return;
  if (!conseq_returns) {
    int conseq_val = b->last_value;
    if (conseq_val >= 0) {
      size_t before = b->nyir.len;
      nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_STORE_LOCAL,
                                           .dst = -1,
                                           .a = conseq_val,
                                           .b = -1,
                                           .imm = result_slot});
      if (b->nyir.len == before)
        return ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
    }
    /* Jump to the merge point (before end_label) so both branches converge
       before the shared load.local. */
    if (!ny_native_nir_emit_br(b, merge_label))
      return false;
  }
  if (!ny_native_nir_emit_label(b, else_label))
    return false;

  /* Else branch. */
  b->emitted_return = false;
  b->last_value = entry_last_value;
  if (has_alt && !ny_native_nir_lower_stmt(b, s->as.iff.alt))
    return false;
  bool alt_returns = b->emitted_return;
  if (!alt_returns && has_alt) {
    int alt_val = b->last_value;
    if (alt_val >= 0) {
      size_t before = b->nyir.len;
      nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_STORE_LOCAL,
                                           .dst = -1,
                                           .a = alt_val,
                                           .b = -1,
                                           .imm = result_slot});
      if (b->nyir.len == before)
        return ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
    }
  }

  /* Merge point: both branches converge here. */
  b->emitted_return = entry_return || (has_alt && conseq_returns && alt_returns);
  if (!b->emitted_return) {
    if (!ny_native_nir_emit_label(b, merge_label))
      return false;
    int loaded = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_LOAD_LOCAL,
                                                       .dst = -1,
                                                       .a = -1,
                                                       .b = -1,
                                                       .imm = result_slot});
    if (loaded < 0)
      return ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
    b->last_value = loaded;
  }
  return ny_native_nir_emit_label(b, end_label);
}

static bool ny_native_nir_lower_while(ny_native_nir_builder_t *b, const stmt_t *s) {
  if (s->as.whl.init && !ny_native_nir_lower_stmt(b, s->as.whl.init))
    return false;
  int head_label = b->next_label++;
  int update_label = s->as.whl.update ? b->next_label++ : head_label;
  int body_label = b->next_label++;
  int end_label = b->next_label++;
  if (!ny_native_nir_emit_label(b, head_label))
    return false;
  int cond = ny_native_nir_lower_expr(b, s->as.whl.test);
  if (cond < 0)
    return false;
  if (!ny_native_nir_emit_br_if(b, cond, body_label) ||
      !ny_native_nir_emit_br(b, end_label) ||
      !ny_native_nir_emit_label(b, body_label))
    return false;
  bool entry_return = b->emitted_return;
  int entry_last_value = b->last_value;
  size_t loop_i = b->loop_depth;
  if (!ny_native_nir_push_loop(b, head_label, update_label, end_label))
    return false;
  b->emitted_return = false;
  bool body_ok = ny_native_nir_lower_stmt(b, s->as.whl.body);
  if (body_ok && s->as.whl.update) {
    b->emitted_return = false;
    body_ok = ny_native_nir_emit_label(b, update_label) &&
              ny_native_nir_lower_stmt(b, s->as.whl.update);
  }
  b->loop_depth = loop_i;
  if (!body_ok)
    return false;
  b->emitted_return = entry_return;
  b->last_value = entry_last_value;
  return ny_native_nir_emit_br(b, head_label) &&
         ny_native_nir_emit_label(b, end_label);
}

static bool ny_native_nir_iterable_is_range(const expr_t *iterable,
                                            const expr_t **lo,
                                            const expr_t **hi) {
  if (!iterable || iterable->kind != NY_E_BINARY || !iterable->as.binary.op ||
      strcmp(iterable->as.binary.op, "..") != 0)
    return false;
  if (lo)
    *lo = iterable->as.binary.left;
  if (hi)
    *hi = iterable->as.binary.right;
  return true;
}

static bool ny_native_nir_lower_for_range(ny_native_nir_builder_t *b,
                                          const stmt_t *s) {
  const expr_t *lo_expr = NULL;
  const expr_t *hi_expr = NULL;
  if (!s->as.fr.iter_var || !ny_native_nir_iterable_is_range(s->as.fr.iterable,
                                                             &lo_expr, &hi_expr)) {
    return ny_native_nir_fail(
        b, "native NYIR lower: for loops currently support `for name in lo..hi` ranges");
  }

  size_t loop_scope_mark = ny_native_nir_scope_mark(b);
  ny_native_nir_local_t *iter = ny_native_nir_bind_local(b, s->as.fr.iter_var);
  if (!iter)
    return false;
  ny_native_nir_local_t *index = NULL;
  if (s->as.fr.iter_index_var) {
    index = ny_native_nir_bind_local(b, s->as.fr.iter_index_var);
    if (!index)
      return false;
  }

  int lo = ny_native_nir_lower_expr(b, lo_expr);
  int hi = ny_native_nir_lower_expr(b, hi_expr);
  int hi_slot = ny_native_nir_temp_slot(b);
  if (lo < 0 || hi < 0 || !ny_native_nir_store_local_value(b, iter->slot, lo) ||
      !ny_native_nir_store_local_value(b, hi_slot, hi))
    return false;
  if (index) {
    int zero = ny_native_nir_emit_const(b, 0);
    if (zero < 0 || !ny_native_nir_store_local_value(b, index->slot, zero))
      return false;
  }

  int head_label = b->next_label++;
  int update_label = b->next_label++;
  int body_label = b->next_label++;
  int end_label = b->next_label++;
  if (!ny_native_nir_emit_label(b, head_label))
    return false;

  int cur = ny_native_nir_load_local_value(b, iter->slot);
  int end = ny_native_nir_load_local_value(b, hi_slot);
  int in_range = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_CMP_I64,
                                                      .dst = -1,
                                                      .a = cur,
                                                      .b = end,
                                                      .cmp = NYIR_CMP_LE});
  if (in_range < 0 || !ny_native_nir_emit_br_if(b, in_range, body_label) ||
      !ny_native_nir_emit_br(b, end_label) ||
      !ny_native_nir_emit_label(b, body_label))
    return false;

  size_t loop_i = b->loop_depth;
  if (!ny_native_nir_push_loop(b, head_label, update_label, end_label))
    return false;
  bool entry_return = b->emitted_return;
  int entry_last_value = b->last_value;
  b->emitted_return = false;
  bool body_ok = ny_native_nir_lower_stmt(b, s->as.fr.body);
  b->loop_depth = loop_i;
  if (!body_ok)
    return false;

  b->emitted_return = false;
  if (!ny_native_nir_emit_label(b, update_label))
    return false;
  int one = ny_native_nir_emit_const(b, 1);
  cur = ny_native_nir_load_local_value(b, iter->slot);
  int next = one >= 0 && cur >= 0
                 ? nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_ADD_I64,
                                                         .dst = -1,
                                                         .a = cur,
                                                         .b = one})
                 : -1;
  if (next < 0 || !ny_native_nir_store_local_value(b, iter->slot, next))
    return false;
  if (index) {
    int old_idx = ny_native_nir_load_local_value(b, index->slot);
    int next_idx = old_idx >= 0
                       ? nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_ADD_I64,
                                                               .dst = -1,
                                                               .a = old_idx,
                                                               .b = one})
                       : -1;
    if (next_idx < 0 || !ny_native_nir_store_local_value(b, index->slot, next_idx))
      return false;
  }

  b->emitted_return = entry_return;
  b->last_value = entry_last_value;
  bool ok = ny_native_nir_emit_br(b, head_label) &&
            ny_native_nir_emit_label(b, end_label);
  ny_native_nir_scope_restore(b, loop_scope_mark);
  return ok;
}

static bool ny_native_nir_lower_for(ny_native_nir_builder_t *b, const stmt_t *s) {
  if (s->as.fr.init || s->as.fr.cond || s->as.fr.update)
    return ny_native_nir_fail(
        b, "native NYIR lower: only Nytrix iterator loops are supported here; use `for name in lo..hi { ... }` because ';' starts a comment");
  return ny_native_nir_lower_for_range(b, s);
}

static bool ny_native_nir_pattern_is_wildcard(const expr_t *pat) {
  return pat && pat->kind == NY_E_IDENT && pat->as.ident.name &&
         strcmp(pat->as.ident.name, "_") == 0;
}

static int ny_native_nir_emit_cmp(ny_native_nir_builder_t *b, int a, int rhs,
                                  nyir_cmp_t cmp) {
  int v = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_CMP_I64,
                                               .dst = -1,
                                               .a = a,
                                               .b = rhs,
                                               .cmp = cmp});
  if (v < 0)
    ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
  return v;
}

static int ny_native_nir_emit_bool_binop(ny_native_nir_builder_t *b,
                                         nyir_op_t op, int a, int rhs) {
  int v = nyir_emit(&b->nyir, (nyir_inst_t){.op = op,
                                               .dst = -1,
                                               .a = a,
                                               .b = rhs});
  if (v < 0)
    ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
  return v;
}

static int ny_native_nir_lower_match_pattern(ny_native_nir_builder_t *b,
                                             int test_value,
                                             const expr_t *pat) {
  if (ny_native_nir_pattern_is_wildcard(pat))
    return ny_native_nir_emit_const(b, 1);

  if (pat && pat->kind == NY_E_BINARY && pat->as.binary.op &&
      strcmp(pat->as.binary.op, "..") == 0) {
    int lo = ny_native_nir_lower_expr(b, pat->as.binary.left);
    int hi = ny_native_nir_lower_expr(b, pat->as.binary.right);
    if (lo < 0 || hi < 0)
      return -1;
    int ge = ny_native_nir_emit_cmp(b, test_value, lo, NYIR_CMP_GE);
    int le = ny_native_nir_emit_cmp(b, test_value, hi, NYIR_CMP_LE);
    if (ge < 0 || le < 0)
      return -1;
    return ny_native_nir_emit_bool_binop(b, NYIR_AND_I64, ge, le);
  }

  int rhs = ny_native_nir_lower_expr(b, pat);
  if (rhs < 0)
    return -1;
  return ny_native_nir_emit_cmp(b, test_value, rhs, NYIR_CMP_EQ);
}

static int ny_native_nir_lower_match_patterns(ny_native_nir_builder_t *b,
                                              int test_value,
                                              const match_arm_t *arm) {
  if (!arm || arm->patterns.len == 0)
    return ny_native_nir_emit_const(b, 0);
  int combined = -1;
  for (size_t i = 0; i < arm->patterns.len; ++i) {
    int cur = ny_native_nir_lower_match_pattern(b, test_value,
                                                arm->patterns.data[i]);
    if (cur < 0)
      return -1;
    combined = combined < 0
                   ? cur
                   : ny_native_nir_emit_bool_binop(b, NYIR_OR_I64,
                                                   combined, cur);
    if (combined < 0)
      return -1;
  }
  return combined;
}

static bool ny_native_nir_lower_match(ny_native_nir_builder_t *b,
                                      const stmt_t *s) {
  if (!s || s->kind != NY_S_MATCH || !s->as.match.test)
    return ny_native_nir_fail(b, "native NYIR lower: malformed case/match");

  int test_value = ny_native_nir_lower_expr(b, s->as.match.test);
  if (test_value < 0)
    return false;

  int result_slot = ny_native_nir_temp_slot(b);
  int merge_label = b->next_label++;
  bool entry_return = b->emitted_return;
  int entry_last_value = b->last_value;
  bool all_taken_paths_return = true;

  int initial = entry_last_value >= 0 ? entry_last_value
                                      : ny_native_nir_emit_const(b, 0);
  if (initial < 0 || !ny_native_nir_store_local_value(b, result_slot, initial))
    return false;

  for (size_t i = 0; i < s->as.match.arms.len; ++i) {
    match_arm_t *arm = &s->as.match.arms.data[i];
    int next_label = b->next_label++;
    int pat = ny_native_nir_lower_match_patterns(b, test_value, arm);
    if (pat < 0)
      return false;
    int pat_false = ny_native_nir_emit_is_zero(b, pat);
    if (pat_false < 0 || !ny_native_nir_emit_br_if(b, pat_false, next_label))
      return false;

    if (arm->guard) {
      int guard = ny_native_nir_lower_expr(b, arm->guard);
      int guard_false = guard >= 0 ? ny_native_nir_emit_is_zero(b, guard) : -1;
      if (guard_false < 0 ||
          !ny_native_nir_emit_br_if(b, guard_false, next_label))
        return false;
    }

    b->emitted_return = false;
    if (!ny_native_nir_lower_stmt(b, arm->conseq))
      return false;
    bool arm_returns = b->emitted_return;
    if (!arm_returns) {
      all_taken_paths_return = false;
      if (b->last_value >= 0 &&
          !ny_native_nir_store_local_value(b, result_slot, b->last_value))
        return false;
      if (!ny_native_nir_emit_br(b, merge_label))
        return false;
    }

    if (!ny_native_nir_emit_label(b, next_label))
      return false;
  }

  if (s->as.match.default_conseq) {
    b->emitted_return = false;
    if (!ny_native_nir_lower_stmt(b, s->as.match.default_conseq))
      return false;
    bool default_returns = b->emitted_return;
    if (!default_returns) {
      all_taken_paths_return = false;
      if (b->last_value >= 0 &&
          !ny_native_nir_store_local_value(b, result_slot, b->last_value))
        return false;
      if (!ny_native_nir_emit_br(b, merge_label))
        return false;
    }
  } else {
    all_taken_paths_return = false;
  }

  b->emitted_return = entry_return || all_taken_paths_return;
  if (!b->emitted_return) {
    if (!ny_native_nir_emit_label(b, merge_label))
      return false;
    int loaded = ny_native_nir_load_local_value(b, result_slot);
    if (loaded < 0)
      return false;
    b->last_value = loaded;
  }
  return true;
}

static bool ny_native_nir_lower_stmt(ny_native_nir_builder_t *b, const stmt_t *s) {
  if (ny_native_nir_ignored_stmt(s) || (s && s->kind == NY_S_FUNC))
    return true;
  switch (s->kind) {
  case NY_S_BLOCK: {
    size_t mark = s->as.block.transparent ? b->local_count
                                           : ny_native_nir_scope_mark(b);
    for (size_t i = 0; i < s->as.block.body.len; ++i) {
      if (!ny_native_nir_lower_stmt(b, s->as.block.body.data[i])) {
        if (!s->as.block.transparent)
          ny_native_nir_scope_restore(b, mark);
        return false;
      }
      if (b->emitted_return)
        break;
    }
    if (!s->as.block.transparent)
      ny_native_nir_scope_restore(b, mark);
    return true;
  }
  case NY_S_VAR:
    return ny_native_nir_lower_var(b, s);
  case NY_S_EXPR: {
    int v = ny_native_nir_lower_expr(b, s->as.expr.expr);
    if (v < 0)
      return false;
    b->last_value = v;
    return true;
  }
  case NY_S_IF:
    return ny_native_nir_lower_if(b, s);
  case NY_S_WHILE:
    return ny_native_nir_lower_while(b, s);
  case NY_S_FOR:
    return ny_native_nir_lower_for(b, s);
  case NY_S_MATCH:
    return ny_native_nir_lower_match(b, s);
  case NY_S_BREAK:
    if (b->loop_depth == 0)
      return ny_native_nir_fail(b, "native NYIR lower: break outside loop");
    b->emitted_return = true;
    return ny_native_nir_emit_br(b,
                                 b->loop_frames[b->loop_depth - 1].end_label);
  case NY_S_CONTINUE:
    if (b->loop_depth == 0)
      return ny_native_nir_fail(b, "native NYIR lower: continue outside loop");
    b->emitted_return = true;
    return ny_native_nir_emit_br(
        b, b->loop_frames[b->loop_depth - 1].continue_label);
  case NY_S_RETURN: {
    int v = s->as.ret.value ? ny_native_nir_lower_expr(b, s->as.ret.value)
                            : ny_native_nir_emit_const(b, 0);
    return v >= 0 && ny_native_nir_emit_ret(b, v);
  }
  default:
    return ny_native_nir_fail(b,
                              "native NYIR lower: statement kind %d is not in shared NYIR yet",
                              (int)s->kind);
  }
}

/*
 * Shared NYIR optimization + verification step.  After calling this the
 * builder's NYIR is ready for codegen or diagnostics.
 */
static bool ny_native_nir_finalize(ny_native_nir_builder_t *b,
                                     char *err, size_t err_len) {
  nyir_opt_stats_t stats;
  /* Initial lowering is a verifier boundary too: no optimization pass should
   * have to defend itself against malformed values, labels, effects, or CFG
   * structure produced upstream. */
  if (!nyir_verify(&b->nyir, err, err_len))
    goto fail;
  ny_native_profile_select(b->profile_name ? b->profile_name : "rt_main");
  if (!nyir_optimize_with_stats(&b->nyir, &stats, b->opt_level)) {
    if (verbose_enabled >= 1)
      fprintf(stderr, "nyir opt FAILED\n");
    if (err && err_len > 0 && err[0] == '\0')
      snprintf(err, err_len, "native NYIR: optimization failed");
    goto fail;
  }
  if (!nyir_verify(&b->nyir, err, err_len))
    goto fail;
  if (verbose_enabled >= 1 && stats.total_time_ms > 0.001) {
    bool grew = stats.after_insts > stats.before_insts;
    size_t delta = grew ? stats.after_insts - stats.before_insts
                        : stats.before_insts - stats.after_insts;
    double pct = stats.before_insts > 0
                     ? (grew ? 1.0 : -1.0) * 100.0 * (double)delta /
                           stats.before_insts
                     : 0.0;
    fprintf(stderr, "nyir finalize: %zu→%zu insts (%c%zu, %+.1f%%) in %.2fms\n",
            stats.before_insts, stats.after_insts, grew ? '+' : '-', delta, pct,
            stats.total_time_ms);
  }
  return true;
fail:
  /* Reduced repro dump on failure. */
  fprintf(stderr, "native NYIR repro (optimize/verify failed): %s\n",
          err && err[0] ? err : "unknown error");
  nyir_dump(stderr, &b->nyir, "<failed>");
  return false;
}

static bool ny_native_nir_opt_dump(FILE *out, ny_native_nir_builder_t *b,
                                   const char *name, const ny_options *opt) {
  nyir_opt_stats_t stats;
  if (!nyir_verify(&b->nyir, b->err, b->err_len)) {
    if (b->err && b->err_len > 0 && b->err[0] == '\0')
      ny_native_set_err(b->err, b->err_len,
                        "native NYIR dump: initial verifier rejected input");
    return false;
  }
  bool optimized = opt && opt->nyir_dump_raw
                       ? nyir_optimize_debug(&b->nyir, out, &stats,
                                               b->opt_level)
                       : nyir_optimize_with_stats(&b->nyir, &stats,
                                                    b->opt_level);
  if (!optimized ||
      !nyir_verify(&b->nyir, b->err, b->err_len)) {
    if (b->err && b->err_len > 0 && b->err[0] == '\0')
      ny_native_set_err(b->err, b->err_len, "native NYIR dump: optimization failed");
    return false;
  }
  if (opt && opt->nyir_dump_stats)
    nyir_dump_stats(out, &stats);
  nyir_dump(out, &b->nyir, name);
  if (opt && opt->nyir_dump_cfg)
    nyir_dump_cfg(out, &b->nyir, name);
  return true;
}

/*
 * Lower a single function stmt into a finalized nyir_func_t.
 * Returns true on success; caller must nyir_func_free(out) when done.
 */
static bool ny_native_nir_build_function(const program_t *prog, const stmt_t *fn,
                                        nyir_func_t *out, char *err,
                                        size_t err_len, int opt_level) {
  if (!fn || fn->kind != NY_S_FUNC || !out)
    return false;
  memset(out, 0, sizeof(*out));
  int function_opt_level = fn->as.fn.attr_optimize
                               ? fn->as.fn.attr_optimize_level
                               : opt_level;
  ny_native_nir_builder_t b = {.last_value = -1, .err = err, .err_len = err_len,
                                .prog = prog, .profile_name = fn->as.fn.name ? fn->as.fn.name : "<fn>",
                                .opt_level = function_opt_level};
  if (ny_native_type_name_is_f64(fn->as.fn.return_type))
    b.return_flags = NYIR_INST_F_RET_F64;
  else if (ny_native_type_name_is_f32(fn->as.fn.return_type))
    b.return_flags = NYIR_INST_F_RET_F32;
  if (!ny_native_nir_set_param_types(&b, fn)) {
    nyir_func_free(&b.nyir);
    ny_native_nir_builder_dispose(&b);
    return false;
  }
  for (size_t i = 0; i < fn->as.fn.params.len; ++i) {
    if (!ny_native_nir_bind_local_typed(
            &b, fn->as.fn.params.data[i].name,
            ny_native_type_name_is_f64(fn->as.fn.params.data[i].type),
            ny_native_type_name_is_f32(fn->as.fn.params.data[i].type))) {
      nyir_func_free(&b.nyir);
      ny_native_nir_builder_dispose(&b);
      return false;
    }
  }
  bool ok = ny_native_nir_lower_stmt(&b, fn->as.fn.body);
  if (ok && !b.emitted_return) {
    int ret = b.last_value >= 0 ? b.last_value : ny_native_nir_emit_const(&b, 0);
    ok = ret >= 0 && ny_native_nir_emit_ret(&b, ret);
  }
  if (ok)
    ok = ny_native_nir_finalize(&b, err, err_len);
  if (ok)
    *out = b.nyir;
  else
    nyir_func_free(&b.nyir);
  ny_native_nir_builder_dispose(&b);
  return ok;
}

/*
 * Build extern table from #include and extern top-level statements.
 * Populates the table with NY name → C symbol mappings so the call
 * lowerer can emit correct linker symbols for extern C functions.
 *
 * Like codegen_collect_links()/process_links(), this recurses through
 * the program: extern declarations are not guaranteed to be top-level.
 * The prelude/stdlib and the script wrapper nest user declarations
 * inside NY_S_MODULE/NY_S_BLOCK (and control-flow bodies), so a flat
 * scan of prog->body would silently miss them.
 */
static bool ny_native_target_platform_ident(const ny_options *opt,
                                             const char *name, bool *out) {
  if (!name || !out)
    return false;
  const char *triple = opt ? opt->host_triple : NULL;
  const char *host_os = ny_host_os_name();
  const char *host_arch = ny_host_arch_name();
  bool is_windows = triple && (strstr(triple, "windows") || strstr(triple, "mingw") ||
                               strstr(triple, "msvc") || strstr(triple, "win32"));
  bool is_macos = triple && (strstr(triple, "apple") || strstr(triple, "darwin") ||
                             strstr(triple, "macos"));
  bool is_linux = triple && strstr(triple, "linux");
  if (!triple || !*triple) {
    is_windows = strcmp(host_os, "windows") == 0;
    is_macos = strcmp(host_os, "macos") == 0;
    is_linux = strcmp(host_os, "linux") == 0;
  }
  if (opt && opt->native_abi == NY_NATIVE_ABI_WIN64)
    is_windows = true, is_macos = false, is_linux = false;

  bool is_x86_64 = false, is_x86 = false, is_aarch64 = false;
  bool is_arm = false, is_riscv = false;
  if (opt) {
    switch (opt->native_backend) {
    case NY_NATIVE_BACKEND_X86_64: is_x86_64 = is_x86 = true; break;
    case NY_NATIVE_BACKEND_X86: is_x86 = true; break;
    case NY_NATIVE_BACKEND_AARCH64: is_aarch64 = is_arm = true; break;
    case NY_NATIVE_BACKEND_ARM: is_arm = true; break;
    case NY_NATIVE_BACKEND_RISCV: is_riscv = true; break;
    default: break;
    }
  }
  if (!is_x86 && !is_aarch64 && !is_arm && !is_riscv) {
    const char *arch = triple && *triple ? triple : host_arch;
    is_x86_64 = strstr(arch, "x86_64") || strstr(arch, "amd64");
    is_x86 = is_x86_64 || strstr(arch, "i386") || strstr(arch, "i686") ||
             strcmp(arch, "x86") == 0;
    is_aarch64 = strstr(arch, "aarch64") || strstr(arch, "arm64");
    is_arm = is_aarch64 || (strstr(arch, "arm") && !strstr(arch, "aarch64"));
    is_riscv = strstr(arch, "riscv") != NULL;
  }
  bool is_unix = !is_windows && (is_linux || is_macos ||
                                  strcmp(host_os, "unknown") != 0);
  const struct { const char *name; bool value; } values[] = {
      {"linux", is_linux}, {"LINUX", is_linux}, {"IS_LINUX", is_linux},
      {"macos", is_macos}, {"mac", is_macos}, {"MACOS", is_macos},
      {"IS_MACOS", is_macos}, {"windows", is_windows},
      {"IS_WINDOWS", is_windows}, {"unix", is_unix}, {"posix", is_unix},
      {"UNIX", is_unix}, {"IS_UNIX", is_unix}, {"x86_64", is_x86_64},
      {"x64", is_x86_64}, {"IS_X86_64", is_x86_64}, {"x86", is_x86},
      {"IS_X86", is_x86}, {"aarch64", is_aarch64}, {"arm64", is_aarch64},
      {"IS_AARCH64", is_aarch64}, {"arm", is_arm}, {"IS_ARM", is_arm},
      {"riscv", is_riscv}, {"IS_RISCV", is_riscv},
  };
  for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
    if (strcmp(name, values[i].name) == 0) {
      *out = values[i].value;
      return true;
    }
  }
  return false;
}

static bool ny_native_target_eval_bool(const ny_options *opt, const expr_t *e,
                                       bool *out) {
  if (!e || !out)
    return false;
  switch (e->kind) {
  case NY_E_LITERAL:
    if (e->as.literal.kind == NY_LIT_BOOL) {
      *out = e->as.literal.as.b;
      return true;
    }
    if (e->as.literal.kind == NY_LIT_INT) {
      *out = e->as.literal.as.i != 0;
      return true;
    }
    return false;
  case NY_E_IDENT:
    return ny_native_target_platform_ident(opt, e->as.ident.name, out);
  case NY_E_UNARY: {
    bool v = false;
    if (!e->as.unary.op || strcmp(e->as.unary.op, "!") != 0 ||
        !ny_native_target_eval_bool(opt, e->as.unary.right, &v))
      return false;
    *out = !v;
    return true;
  }
  case NY_E_LOGICAL: {
    bool l = false, r = false;
    if (!e->as.logical.op ||
        !ny_native_target_eval_bool(opt, e->as.logical.left, &l))
      return false;
    if (strcmp(e->as.logical.op, "&&") == 0) {
      if (!l) { *out = false; return true; }
      if (!ny_native_target_eval_bool(opt, e->as.logical.right, &r)) return false;
      *out = r; return true;
    }
    if (strcmp(e->as.logical.op, "||") == 0) {
      if (l) { *out = true; return true; }
      if (!ny_native_target_eval_bool(opt, e->as.logical.right, &r)) return false;
      *out = r; return true;
    }
    return false;
  }
  case NY_E_COMPTIME: {
    const stmt_t *body = e->as.comptime_expr.body;
    if (!body)
      return false;
    if (body->kind == NY_S_BLOCK && body->as.block.body.len == 1)
      body = body->as.block.body.data[0];
    return body && body->kind == NY_S_RETURN &&
           ny_native_target_eval_bool(opt, body->as.ret.value, out);
  }
  default:
    return false;
  }
}

static bool ny_native_nir_collect_extern(const stmt_t *s, ny_extern_table_t *t,
                                         bool aapcs, const ny_options *opt,
                                         char *err, size_t err_len) {
  if (!s)
    return true;
  if (s->kind == NY_S_EXTERN) {
    const char *ny_name = s->as.ext.name;
    const char *c_sym = s->as.ext.link_name ? s->as.ext.link_name : ny_name;
    unsigned pc = (unsigned)s->as.ext.params.len;
    if (!ny_extern_table_add(t, ny_name, c_sym, pc, false, 0, NULL, NULL)) {
      if (err && err_len > 0)
        snprintf(err, err_len,
                 "NYIR extern: conflicting or duplicate extern '%s' "
                 "(C symbol '%s' conflicts with earlier declaration)",
                 ny_name, c_sym);
      return false;
    }
    return true;
  }
  if (s->kind == NY_S_INCLUDE) {
    const char *prefix = s->as.inc.prefix;
    char *src = ny_read_file(s->as.inc.path);
    if (!src)
      return true;
    size_t srclen = strlen(src);
    ny_parser_t parser;
    ny_parse_init(&parser, src, srclen);
    ny_cdecl_t decl;
    while (ny_parse_decl(&parser, &decl) > 0) {
      if (decl.kind != NY_CDECL_FUNC)
        continue;
      size_t nlen = decl.name.len;
      char cname[256];
      if (nlen >= sizeof(cname))
        nlen = sizeof(cname) - 1;
      memcpy(cname, decl.name.start, nlen);
      cname[nlen] = '\0';
      char ny_name[512];
      if (prefix && prefix[0]) {
        int nn = snprintf(ny_name, sizeof(ny_name), "%s.%s", prefix, cname);
        if (nn < 0 || (size_t)nn >= sizeof(ny_name))
          continue;
      } else {
        size_t nn = nlen;
        if (nn >= sizeof(ny_name))
          nn = sizeof(ny_name) - 1;
        memcpy(ny_name, cname, nn);
        ny_name[nn] = '\0';
      }
      /* Compute aggregate return size and per-argument aggregate sizes */
      uint32_t ret_agg = 0;
      ny_sysv_agg_class_t ret_agg_classes[2] = {NY_SYSV_AGG_NONE,
                                                NY_SYSV_AGG_NONE};
      if (decl.type.kind == NY_CTYPE_STRUCT || decl.type.kind == NY_CTYPE_UNION) {
        ret_agg = (uint32_t)decl.type.aggregate_size;
      } else if (decl.type.kind == NY_CTYPE_NAMED && decl.type.ptr_depth == 0) {
        /* Named typedef that may be a struct — aggregate_size if present */
        ret_agg = (uint32_t)decl.type.aggregate_size;
      }
      if (ret_agg > 0 &&
          !(aapcs ? ny_native_aapcs_classify_aggregate(&parser, &decl.type,
                                                        ret_agg_classes)
                   : ny_native_sysv_classify_aggregate(&parser, &decl.type,
                                                       ret_agg_classes)))
        ret_agg_classes[0] = NY_SYSV_AGG_UNSUPPORTED;
      uint32_t arg_agg[NY_C_MAX_PARAMS] = {0};
      for (unsigned pi = 0; pi < decl.param_count && pi < NY_C_MAX_PARAMS; pi++) {
        const ny_ctype_t *pt = &decl.params[pi];
        if ((pt->kind == NY_CTYPE_STRUCT || pt->kind == NY_CTYPE_UNION) &&
            pt->ptr_depth == 0) {
          arg_agg[pi] = (uint32_t)pt->aggregate_size;
        } else if (pt->kind == NY_CTYPE_NAMED && pt->ptr_depth == 0) {
          arg_agg[pi] = (uint32_t)pt->aggregate_size;
        }
        if (arg_agg[pi] > 0) {
          ny_sysv_agg_class_t classes[2] = {NY_SYSV_AGG_NONE,
                                            NY_SYSV_AGG_NONE};
          if (arg_agg[pi] > NYIR_ARG_AGG_SIZE_MASK ||
              !(aapcs ? ny_native_aapcs_classify_aggregate(&parser, pt, classes)
                       : ny_native_sysv_classify_aggregate(&parser, pt,
                                                           classes))) {
            classes[0] = NY_SYSV_AGG_UNSUPPORTED;
            classes[1] = NY_SYSV_AGG_NONE;
          }
          arg_agg[pi] =
              (arg_agg[pi] & NYIR_ARG_AGG_SIZE_MASK) |
              ((uint32_t)classes[0] << NYIR_ARG_AGG_CLASS0_SHIFT) |
              ((uint32_t)classes[1] << NYIR_ARG_AGG_CLASS1_SHIFT);
        }
      }
      char *ny_name_dup = ny_strdup(ny_name);
      char *c_sym = ny_strdup(cname);
      if (!ny_name_dup || !c_sym ||
          !ny_extern_table_add(t, ny_name_dup, c_sym, decl.param_count, true,
                               ret_agg, ret_agg_classes, arg_agg)) {
        free(ny_name_dup);
        free(c_sym);
        free(src);
        if (err && err_len > 0)
          snprintf(err, err_len, "NYIR extern: table full from #include");
        return false;
      }
      if (!prefix || !prefix[0]) {
        char default_name[512];
        int nn = snprintf(default_name, sizeof(default_name), "c.%s", cname);
        char *default_name_dup =
            nn > 0 && (size_t)nn < sizeof(default_name)
                ? ny_strdup(default_name)
                : NULL;
        char *default_c_sym = ny_strdup(cname);
        if (!default_name_dup || !default_c_sym ||
            !ny_extern_table_add(t, default_name_dup, default_c_sym,
                                 decl.param_count, true, ret_agg,
                                 ret_agg_classes, arg_agg)) {
          free(default_name_dup);
          free(default_c_sym);
          free(src);
          if (err && err_len > 0)
            snprintf(err, err_len,
                     "NYIR extern: table full from default C namespace");
          return false;
        }
      }
    }
    ny_parse_cleanup(&parser);
    free(src);
    return true;
  }
  /* Recurse through container statements, mirroring process_links(). */
  if (s->kind == NY_S_MODULE) {
    for (size_t i = 0; i < s->as.module.body.len; ++i)
      if (!ny_native_nir_collect_extern(s->as.module.body.data[i], t, aapcs, opt, err, err_len))
        return false;
    return true;
  }
  if (s->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      if (!ny_native_nir_collect_extern(s->as.block.body.data[i], t, aapcs, opt, err, err_len))
        return false;
    return true;
  }
  if (s->kind == NY_S_IF) {
    bool selected = false;
    if (s->as.iff.test && s->as.iff.test->kind == NY_E_COMPTIME &&
        ny_native_target_eval_bool(opt, s->as.iff.test, &selected)) {
      const stmt_t *branch = selected ? s->as.iff.conseq : s->as.iff.alt;
      return !branch || ny_native_nir_collect_extern(branch, t, aapcs, opt,
                                                      err, err_len);
    }
    if (s->as.iff.conseq &&
        !ny_native_nir_collect_extern(s->as.iff.conseq, t, aapcs, opt, err, err_len))
      return false;
    if (s->as.iff.alt &&
        !ny_native_nir_collect_extern(s->as.iff.alt, t, aapcs, opt, err, err_len))
      return false;
    return true;
  }
  if (s->kind == NY_S_WHILE) {
    if (s->as.whl.body &&
        !ny_native_nir_collect_extern(s->as.whl.body, t, aapcs, opt, err, err_len))
      return false;
    if (s->as.whl.update &&
        !ny_native_nir_collect_extern(s->as.whl.update, t, aapcs, opt, err, err_len))
      return false;
    if (s->as.whl.init &&
        !ny_native_nir_collect_extern(s->as.whl.init, t, aapcs, opt, err, err_len))
      return false;
    return true;
  }
  if (s->kind == NY_S_FOR) {
    if (s->as.fr.init &&
        !ny_native_nir_collect_extern(s->as.fr.init, t, aapcs, opt, err, err_len))
      return false;
    if (s->as.fr.body &&
        !ny_native_nir_collect_extern(s->as.fr.body, t, aapcs, opt, err, err_len))
      return false;
    if (s->as.fr.update &&
        !ny_native_nir_collect_extern(s->as.fr.update, t, aapcs, opt, err, err_len))
      return false;
    return true;
  }
  if (s->kind == NY_S_TRY) {
    if (s->as.tr.body &&
        !ny_native_nir_collect_extern(s->as.tr.body, t, aapcs, opt, err, err_len))
      return false;
    if (s->as.tr.handler &&
        !ny_native_nir_collect_extern(s->as.tr.handler, t, aapcs, opt, err, err_len))
      return false;
    return true;
  }
  if (s->kind == NY_S_DEFER) {
    if (s->as.de.body &&
        !ny_native_nir_collect_extern(s->as.de.body, t, aapcs, opt, err, err_len))
      return false;
    return true;
  }
  if (s->kind == NY_S_MATCH) {
    for (size_t i = 0; i < s->as.match.arms.len; ++i)
      if (s->as.match.arms.data[i].conseq &&
          !ny_native_nir_collect_extern(s->as.match.arms.data[i].conseq, t,
                                        aapcs, opt, err, err_len))
        return false;
    if (s->as.match.default_conseq &&
        !ny_native_nir_collect_extern(s->as.match.default_conseq, t, aapcs, opt, err, err_len))
      return false;
    return true;
  }
  return true;
}

static bool ny_native_nir_build_extern_table(const program_t *prog,
                                              ny_extern_table_t *t,
                                              bool aapcs,
                                              const ny_options *opt, char *err,
                                              size_t err_len) {
  if (!t)
    return false;
  ny_extern_table_init(t);
  if (!prog)
    return true;
  for (size_t i = 0; i < prog->body.len; ++i) {
    const stmt_t *s = prog->body.data[i];
    if (!ny_native_nir_collect_extern(s, t, aapcs, opt, err, err_len))
      return false;
  }
  return true;
}

/* Per-pass oracle callback state.  The IR layer calls this after every
 * successful verifier checkpoint while optimizing rt_main. */
typedef struct {
  const ny_options *opt;
  nyir_func_t *funcs;
  const char **names;
  size_t count;
} ny_native_oracle_ctx_t;

static bool ny_native_per_pass_oracle_cb(const nyir_func_t *f,
                                         const char *pass_name,
                                         void *userdata) {
  ny_native_oracle_ctx_t *ctx = (ny_native_oracle_ctx_t *)userdata;
  char err[512] = {0};
  bool ok = ny_native_result_oracle_for_nir(
      (nyir_func_t *)f, ctx->funcs, ctx->names, ctx->count, ctx->opt, err,
      sizeof(err));
  if (!ok) {
    fprintf(stderr, "native NYIR: per-pass oracle failed after %s: %s\n",
            pass_name ? pass_name : "pass", err[0] ? err : NY_NATIVE_UNKNOWN_ERR);
  }
  return ok;
}

/*
 * Lower the top-level program statements into a finalized nyir_func_t for
 * rt_main.  Returns true on success; caller must nyir_func_free(out).
 */
static bool ny_native_nir_build_rt_main(const program_t *prog, nyir_func_t *out,
                                        const ny_extern_table_t *externs,
                                        char *err, size_t err_len, int opt_level) {
  if (!out)
    return false;
  memset(out, 0, sizeof(*out));
  ny_native_nir_builder_t b = {.last_value = -1, .err = err, .err_len = err_len,
                                .externs = externs, .prog = prog, .profile_name = "rt_main",
                                .opt_level = opt_level};
  for (size_t i = 0; prog && i < prog->body.len; ++i) {
    if (!ny_native_nir_lower_stmt(&b, prog->body.data[i])) {
      nyir_func_free(&b.nyir);
      ny_native_nir_builder_dispose(&b);
      return false;
    }
    if (b.emitted_return)
      break;
  }
  if (!b.emitted_return) {
    if (b.last_value < 0) {
      ny_native_nir_fail(&b, "native NYIR: program has no raw expression result");
      nyir_func_free(&b.nyir);
      ny_native_nir_builder_dispose(&b);
      return false;
    }
    if (!ny_native_nir_emit_ret(&b, b.last_value)) {
      nyir_func_free(&b.nyir);
      ny_native_nir_builder_dispose(&b);
      return false;
    }
  }
  bool ok = ny_native_nir_finalize(&b, err, err_len);
  if (ok)
    *out = b.nyir;
  else
    nyir_func_free(&b.nyir);
  ny_native_nir_builder_dispose(&b);
  return ok;
}

bool ny_native_build_nir(const program_t *prog, const ny_options *opt,
                         nyir_func_t *rt_main_out,
                         nyir_func_t *funcs_out, size_t *func_count,
                         size_t max_funcs, char *err, size_t err_len) {
  if (!prog || !rt_main_out)
    return false;
  ny_native_strtab_clear();
  ny_native_profile_clear();
  int opt_level = opt ? opt->opt_level : 1;
  nyir_set_cf_mem2reg_enabled(!opt || opt->native_enable_cf_mem2reg);
  nyir_set_pass_controls(opt ? opt->nyir_disable_pass : NULL,
                           opt ? opt->nyir_stop_after : NULL);
  nyir_set_verify_each_pass(opt && opt->nyir_verify);
  nyir_set_tv_seed(opt ? opt->native_tv_seed_trials : 0);
  char profile_err[256] = {0};
  if (opt && opt->native_profile_use_path &&
      !ny_native_profile_load_path(opt->native_profile_use_path, profile_err,
                                   sizeof(profile_err))) {
    ny_native_set_err(err, err_len, "native PGO: %s", profile_err);
    return false;
  }
  memset(rt_main_out, 0, sizeof(*rt_main_out));
  if (func_count)
    *func_count = 0;

  /* Build extern table from #include and extern statements. */
  ny_extern_table_t externs;
  bool aapcs = opt &&
               (opt->native_backend == NY_NATIVE_BACKEND_AARCH64 ||
                opt->native_abi == NY_NATIVE_ABI_AAPCS);
  char extern_err[256] = {0};
  if (!ny_native_nir_build_extern_table(prog, &externs, aapcs, opt, extern_err,
                                        sizeof(extern_err))) {
    ny_native_set_err(err, err_len, "NYIR extern: %s", extern_err);
    ny_extern_table_free(&externs);
    ny_native_profile_clear();
    return false;
  }

  /* Build user functions first. */
  const char *func_names[128];
  if (funcs_out && func_count && max_funcs > 0) {
    size_t wanted = 0;
    for (size_t i = 0; i < prog->body.len; ++i) {
      const stmt_t *s = prog->body.data[i];
      if (s && s->kind == NY_S_FUNC)
        wanted++;
    }
    if (wanted > max_funcs) {
      ny_native_set_err(err, err_len,
                        "native NYIR build: %zu functions exceed live capacity %zu",
                        wanted, max_funcs);
      ny_extern_table_free(&externs);
      ny_native_profile_clear();
      return false;
    }
    size_t count = 0;
    for (size_t i = 0; i < prog->body.len; ++i) {
      const stmt_t *s = prog->body.data[i];
      if (!s || s->kind != NY_S_FUNC)
        continue;
      char local_err[256] = {0};
      if (!ny_native_nir_build_function(prog, s, &funcs_out[count], local_err,
                                       sizeof(local_err), opt_level)) {
        /* If lowering failed, free any already-built functions. */
        for (size_t j = 0; j < count; ++j)
          nyir_func_free(&funcs_out[j]);
        *func_count = 0;
        /* Non-fatal: function NYIR build failure is not an error for
         * diagnostics-only mode.  Fall through to rt_main. */
        if (err && err_len > 0 && local_err[0])
          ny_native_set_err(err, err_len, "%s", local_err);
      } else {
        func_names[count] = s->as.fn.name ? s->as.fn.name : "<fn>";
        count++;
      }
    }
    *func_count = count;
  }

  /* Offer user functions as inlining candidates for rt_main. */
  nyir_inline_callee_t inline_callees[128];
  size_t inline_count = 0;
  if (funcs_out && func_count && *func_count > 0) {
    for (size_t i = 0; i < *func_count && inline_count < 128; ++i) {
      if (!func_names[i])
        continue;
      inline_callees[inline_count].name = func_names[i];
      inline_callees[inline_count].func = &funcs_out[i];
      inline_count++;
    }
    nyir_set_inline_callees(inline_callees, inline_count);
  }

  /* Build rt_main with extern table.  When requested, run the VM/native result
   * oracle after every optimization pass on rt_main so any pass-level bug is
   * caught before final codegen. */
  ny_native_oracle_ctx_t oracle_ctx = {opt, funcs_out, (const char **)func_names,
                                       func_count ? *func_count : 0};
  if (opt && opt->native_oracle_per_pass && funcs_out && func_count)
    nyir_set_per_pass_oracle(true, ny_native_per_pass_oracle_cb, &oracle_ctx);
  bool ok = ny_native_nir_build_rt_main(prog, rt_main_out, &externs, err, err_len, opt_level);
  nyir_set_per_pass_oracle(false, NULL, NULL);
  nyir_set_inline_callees(NULL, 0);
  nyir_set_tv_seed(0);
  ny_extern_table_free(&externs);
  ny_native_profile_clear();
  return ok;
}

bool ny_native_nir_dump_function(FILE *out, const stmt_t *fn, char *err,
                                 size_t err_len, const ny_options *opt) {
  if (!fn || fn->kind != NY_S_FUNC)
    return true;
  nyir_set_cf_mem2reg_enabled(!opt || opt->native_enable_cf_mem2reg);
  nyir_set_pass_controls(opt ? opt->nyir_disable_pass : NULL,
                           opt ? opt->nyir_stop_after : NULL);
  nyir_set_verify_each_pass(opt && opt->nyir_verify);
  nyir_set_tv_seed(opt ? opt->native_tv_seed_trials : 0);
  ny_native_nir_builder_t b = {.last_value = -1, .err = err, .err_len = err_len,
                                .opt_level = opt ? opt->opt_level : 1};
  if (ny_native_type_name_is_f64(fn->as.fn.return_type))
    b.return_flags = NYIR_INST_F_RET_F64;
  else if (ny_native_type_name_is_f32(fn->as.fn.return_type))
    b.return_flags = NYIR_INST_F_RET_F32;
  if (!ny_native_nir_set_param_types(&b, fn)) {
    nyir_func_free(&b.nyir);
    ny_native_nir_builder_dispose(&b);
    return false;
  }
  for (size_t i = 0; i < fn->as.fn.params.len; ++i) {
    if (!ny_native_nir_bind_local_typed(
            &b, fn->as.fn.params.data[i].name,
            ny_native_type_name_is_f64(fn->as.fn.params.data[i].type),
            ny_native_type_name_is_f32(fn->as.fn.params.data[i].type))) {
      nyir_func_free(&b.nyir);
      ny_native_nir_builder_dispose(&b);
      return false;
    }
  }
  bool ok = ny_native_nir_lower_stmt(&b, fn->as.fn.body);
  if (ok && !b.emitted_return) {
    int ret = b.last_value >= 0 ? b.last_value : ny_native_nir_emit_const(&b, 0);
    ok = ret >= 0 && ny_native_nir_emit_ret(&b, ret);
  }
  if (ok)
    ok = ny_native_nir_opt_dump(out, &b,
                                fn->as.fn.name ? fn->as.fn.name : "<fn>", opt);
  nyir_func_free(&b.nyir);
  ny_native_nir_builder_dispose(&b);
  return ok;
}

bool ny_native_nir_dump_rt_main(FILE *out, const program_t *prog, char *err,
                                size_t err_len, const ny_options *opt) {
  nyir_set_cf_mem2reg_enabled(!opt || opt->native_enable_cf_mem2reg);
  nyir_set_pass_controls(opt ? opt->nyir_disable_pass : NULL,
                           opt ? opt->nyir_stop_after : NULL);
  nyir_set_verify_each_pass(opt && opt->nyir_verify);
  nyir_set_tv_seed(opt ? opt->native_tv_seed_trials : 0);
  ny_extern_table_t externs;
  bool aapcs = opt &&
               (opt->native_backend == NY_NATIVE_BACKEND_AARCH64 ||
                opt->native_abi == NY_NATIVE_ABI_AAPCS);
  if (!ny_native_nir_build_extern_table(prog, &externs, aapcs, opt, err, err_len)) {
    ny_extern_table_free(&externs);
    return false;
  }
  ny_native_nir_builder_t b = {.last_value = -1,
                                .err = err,
                                .err_len = err_len,
                                .externs = &externs,
                                .prog = prog,
                                .opt_level = opt ? opt->opt_level : 1};
  for (size_t i = 0; prog && i < prog->body.len; ++i) {
    if (!ny_native_nir_lower_stmt(&b, prog->body.data[i])) {
      nyir_func_free(&b.nyir);
      ny_native_nir_builder_dispose(&b);
      ny_extern_table_free(&externs);
      return false;
    }
    if (b.emitted_return)
      break;
  }
  if (!b.emitted_return) {
    if (b.last_value < 0) {
      ny_native_nir_fail(&b, "native NYIR dump unavailable: program has no raw expression result");
      nyir_func_free(&b.nyir);
      ny_native_nir_builder_dispose(&b);
      ny_extern_table_free(&externs);
      return false;
    }
    if (!ny_native_nir_emit_ret(&b, b.last_value)) {
      nyir_func_free(&b.nyir);
      ny_native_nir_builder_dispose(&b);
      ny_extern_table_free(&externs);
      return false;
    }
  }
  bool ok = ny_native_nir_opt_dump(out, &b, "rt_main", opt);
  nyir_func_free(&b.nyir);
  ny_native_nir_builder_dispose(&b);
  ny_extern_table_free(&externs);
  return ok;
}

size_t ny_native_nir_local_count(const nyir_func_t *f);
bool ny_native_ensure_parent_dir_for_path(const char *path);

static bool ny_native_nir_write_u16le(FILE *out, uint16_t value) {
  unsigned char bytes[2] = {(unsigned char)value, (unsigned char)(value >> 8)};
  return fwrite(bytes, 1, sizeof(bytes), out) == sizeof(bytes);
}

static bool ny_native_nir_write_u32le(FILE *out, uint32_t value) {
  unsigned char bytes[4] = {(unsigned char)value, (unsigned char)(value >> 8),
                            (unsigned char)(value >> 16), (unsigned char)(value >> 24)};
  return fwrite(bytes, 1, sizeof(bytes), out) == sizeof(bytes);
}

static bool ny_native_nir_write_binary_blob(FILE *out, const nyir_func_t *nyir,
                                            const char *name, char *err,
                                            size_t err_len) {
  FILE *tmp = tmpfile();
  if (!tmp)
    return ny_native_set_err(err, err_len, "native NYIR bundle: failed to create temporary blob"), false;
  bool ok = nyir_dump_binary(tmp, nyir, name);
  long bytes = ok && fflush(tmp) == 0 && fseek(tmp, 0, SEEK_END) == 0 ? ftell(tmp) : -1;
  if (!ok || bytes < 0 || (unsigned long)bytes > UINT32_MAX || fseek(tmp, 0, SEEK_SET) != 0 ||
      !ny_native_nir_write_u32le(out, (uint32_t)bytes)) {
    fclose(tmp);
    return ny_native_set_err(err, err_len, "native NYIR bundle: failed to encode %s", name), false;
  }
  unsigned char buffer[4096];
  size_t left = (size_t)bytes;
  while (left > 0) {
    size_t want = left < sizeof(buffer) ? left : sizeof(buffer);
    size_t got = fread(buffer, 1, want, tmp);
    if (got != want || fwrite(buffer, 1, got, out) != got) {
      fclose(tmp);
      return ny_native_set_err(err, err_len, "native NYIR bundle: failed to write %s", name), false;
    }
    left -= got;
  }
  fclose(tmp);
  return true;
}

bool ny_native_nir_dump_program_binary(FILE *out, const program_t *prog,
                                       const ny_options *opt, char *err,
                                       size_t err_len) {
  if (!out || !prog)
    return ny_native_set_err(err, err_len, "native NYIR bundle: missing program output"), false;
  enum { NY_NATIVE_NIR_BUNDLE_VERSION = 1 };
  nyir_func_t rt_main = {0};
  size_t wanted = 0;
  for (size_t i = 0; i < prog->body.len; ++i) {
    const stmt_t *stmt = prog->body.data[i];
    if (stmt && stmt->kind == NY_S_FUNC)
      wanted++;
  }
  if (wanted > NY_NATIVE_NIR_BUNDLE_MAX_FUNCS)
    return ny_native_set_err(err, err_len,
                             "native NYIR bundle: %zu functions exceed bundle limit %u",
                             wanted, NY_NATIVE_NIR_BUNDLE_MAX_FUNCS), false;
  nyir_func_t *funcs = wanted ? calloc(wanted, sizeof(*funcs)) : NULL;
  const char **names = wanted ? calloc(wanted, sizeof(*names)) : NULL;
  if (wanted && (!funcs || !names)) {
    free(funcs);
    free(names);
    return ny_native_set_err(err, err_len, NY_NATIVE_BUNDLE_OOM), false;
  }
  size_t named = 0;
  for (size_t i = 0; i < prog->body.len; ++i) {
    const stmt_t *stmt = prog->body.data[i];
    if (stmt && stmt->kind == NY_S_FUNC)
      names[named++] = stmt->as.fn.name ? stmt->as.fn.name : "<fn>";
  }
  size_t count = 0;
  bool ok = ny_native_build_nir(prog, opt, &rt_main, funcs, &count,
                                wanted, err, err_len);
  if (!ok || count != wanted) {
    if (ok)
      ny_native_set_err(err, err_len,
                        "native NYIR bundle: every user function must lower before serialization");
    ok = false;
    goto done;
  }
  if (fwrite("NYIP", 1, 4, out) != 4 ||
      !ny_native_nir_write_u16le(out, NY_NATIVE_NIR_BUNDLE_VERSION) ||
      !ny_native_nir_write_u16le(out, 0) ||
      !ny_native_nir_write_u32le(out, (uint32_t)(count + 1)) ||
      !ny_native_nir_write_binary_blob(out, &rt_main, "rt_main", err, err_len)) {
    ok = false;
    goto done;
  }
  for (size_t i = 0; i < count; ++i)
    if (!ny_native_nir_write_binary_blob(out, &funcs[i], names[i], err, err_len)) {
      ok = false;
      goto done;
    }
done:
  nyir_func_free(&rt_main);
  for (size_t i = 0; i < count; ++i)
    nyir_func_free(&funcs[i]);
  free(funcs);
  free(names);
  return ok;
}

bool ny_native_write_nir_metadata_report(const program_t *prog,
                                         const ny_options *opt, char *err,
                                         size_t err_len) {
  if (!opt || !opt->nyir_metadata_report)
    return true;
  FILE *out = stderr;
  if (opt->nyir_metadata_report_path && opt->nyir_metadata_report_path[0]) {
    ny_native_ensure_parent_dir_for_path(opt->nyir_metadata_report_path);
    out = fopen(opt->nyir_metadata_report_path, "wb");
    if (!out) {
      ny_native_set_err(err, err_len,
                        "native NYIR metadata: failed to open %s: %s",
                        opt->nyir_metadata_report_path, strerror(errno));
      return false;
    }
  }

  if (opt->nyir_metadata_bin_path && opt->nyir_metadata_bin_path[0]) {
    FILE *in = fopen(opt->nyir_metadata_bin_path, "rb");
    if (!in) {
      if (out != stderr)
        fclose(out);
      ny_native_set_err(err, err_len,
                        "native NYIR metadata: failed to open %s: %s",
                        opt->nyir_metadata_bin_path, strerror(errno));
      return false;
    }
    nyir_func_t f = {0};
    char name[128] = {0};
    char local_err[512] = {0};
    bool ok = nyir_load_binary(in, &f, name, sizeof(name), local_err,
                                 sizeof(local_err));
    fclose(in);
    if (ok) {
      nyir_metadata_summary_t summary = {0};
      ok = nyir_metadata_summary(&f, &summary, local_err,
                                   sizeof(local_err));
      if (ok) {
        fprintf(out, "nyir metadata report functions=1 source=binary path=%s\n",
                opt->nyir_metadata_bin_path);
        nyir_metadata_summary_dump(out, name[0] ? name : "rt_main",
                                     &summary);
      }
    }
    nyir_func_free(&f);
    if (out != stderr)
      fclose(out);
    if (!ok) {
      ny_native_set_err(err, err_len, "%s",
                        local_err[0] ? local_err
                                     : "native NYIR binary metadata failed");
      return false;
    }
    if (err && err_len > 0)
      err[0] = '\0';
    return true;
  }

  nyir_func_t rt_main = {0};
  nyir_func_t funcs[NY_NATIVE_LIVE_MAX_FUNCS] = {{0}};
  const char *names[NY_NATIVE_LIVE_MAX_FUNCS] = {0};
  size_t wanted_names = 0;
  for (size_t i = 0; prog && i < prog->body.len &&
                     wanted_names < NY_NATIVE_LIVE_MAX_FUNCS; ++i) {
    const stmt_t *s = prog->body.data[i];
    if (s && s->kind == NY_S_FUNC)
      names[wanted_names++] = s->as.fn.name ? s->as.fn.name : "<fn>";
  }

  size_t func_count = 0;
  char local_err[512] = {0};
  bool ok = ny_native_build_nir(prog, opt, &rt_main, funcs, &func_count,
                                NY_NATIVE_LIVE_MAX_FUNCS,
                                local_err, sizeof(local_err));
  if (!ok) {
    if (out != stderr)
      fclose(out);
    ny_native_set_err(err, err_len, "%s",
                      local_err[0] ? local_err : "native NYIR build failed");
    return false;
  }

  fprintf(out, "nyir metadata report functions=%zu\n", func_count + 1);
  for (size_t i = 0; i < func_count; ++i) {
    nyir_metadata_summary_t summary = {0};
    if (!nyir_metadata_summary(&funcs[i], &summary, local_err,
                                 sizeof(local_err))) {
      ok = false;
      break;
    }
    nyir_metadata_summary_dump(out, i < wanted_names ? names[i] : "<fn>",
                                 &summary);
  }
  if (ok) {
    nyir_metadata_summary_t summary = {0};
    if (nyir_metadata_summary(&rt_main, &summary, local_err,
                                sizeof(local_err)))
      nyir_metadata_summary_dump(out, "rt_main", &summary);
    else
      ok = false;
  }

  nyir_func_free(&rt_main);
  for (size_t i = 0; i < func_count; ++i)
    nyir_func_free(&funcs[i]);
  if (out != stderr)
    fclose(out);
  if (!ok) {
    ny_native_set_err(err, err_len, "%s",
                      local_err[0] ? local_err : "native NYIR metadata failed");
    return false;
  }
  if (err && err_len > 0)
    err[0] = '\0';
  return true;
}
