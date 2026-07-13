#include "code/native/internal.h"
#include "code/c/c.h"
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

/* AST-to-NYIR lowering, extern discovery, optimized construction, dumps, and
 * metadata summaries. Execution and target emission live in other modules. */

static bool ny_native_nir_binop(const char *op, ny_nir_op_t *out) {
  if (!op || !out)
    return false;
  if (strcmp(op, "+") == 0)
    *out = NY_NIR_ADD_I64;
  else if (strcmp(op, "-") == 0)
    *out = NY_NIR_SUB_I64;
  else if (strcmp(op, "*") == 0)
    *out = NY_NIR_MUL_I64;
  else if (strcmp(op, "/") == 0)
    *out = NY_NIR_DIV_I64;
  else if (strcmp(op, "%") == 0)
    *out = NY_NIR_MOD_I64;
  else if (strcmp(op, "&") == 0)
    *out = NY_NIR_AND_I64;
  else if (strcmp(op, "|") == 0)
    *out = NY_NIR_OR_I64;
  else if (strcmp(op, "^^") == 0)
    *out = NY_NIR_XOR_I64;
  else if (strcmp(op, "<<") == 0)
    *out = NY_NIR_SHL_I64;
  else if (strcmp(op, ">>") == 0)
    *out = NY_NIR_SAR_I64;
  else
    return false;
  return true;
}

static bool ny_native_nir_cmp(const char *op, ny_nir_cmp_t *out) {
  if (!op || !out)
    return false;
  if (strcmp(op, "==") == 0)
    *out = NY_NIR_CMP_EQ;
  else if (strcmp(op, "!=") == 0)
    *out = NY_NIR_CMP_NE;
  else if (strcmp(op, "<") == 0)
    *out = NY_NIR_CMP_LT;
  else if (strcmp(op, "<=") == 0)
    *out = NY_NIR_CMP_LE;
  else if (strcmp(op, ">") == 0)
    *out = NY_NIR_CMP_GT;
  else if (strcmp(op, ">=") == 0)
    *out = NY_NIR_CMP_GE;
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

#define NY_EXTERN_MAX 256

typedef enum {
  NY_SYSV_AGG_NONE = 0,
  NY_SYSV_AGG_INTEGER,
  NY_SYSV_AGG_SSE,
  NY_SYSV_AGG_MEMORY,
  NY_SYSV_AGG_UNSUPPORTED,
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
  if (t->count >= NY_EXTERN_MAX)
    return false;
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
  ny_nir_func_t nir;
  ny_native_nir_local_t locals[256];
  size_t local_count;
  int next_local_slot;
  int next_label;
  int last_value;
  int loop_head_labels[64];
  int loop_continue_labels[64];
  int loop_end_labels[64];
  size_t loop_depth;
  bool emitted_return;
  const ny_extern_table_t *externs;
  const program_t *prog;
  char *err;
  size_t err_len;
} ny_native_nir_builder_t;

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

static bool ny_native_nir_fail(ny_native_nir_builder_t *b, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

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

static bool ny_native_type_name_is_float(const char *name) {
  return ny_native_type_name_is_f64(name) || ny_native_type_name_is_f32(name);
}

static int64_t ny_native_f64_bits(double v) {
  int64_t bits = 0;
  memcpy(&bits, &v, sizeof(bits));
  return bits;
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
  if (b->local_count >= sizeof(b->locals) / sizeof(b->locals[0])) {
    ny_native_nir_fail(b, "native NYIR lower: local limit exceeded");
    return NULL;
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

static bool ny_native_nir_emit_label(ny_native_nir_builder_t *b, int label) {
  size_t before = b->nir.len;
  ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NY_NIR_LABEL,
                                       .dst = -1,
                                       .a = -1,
                                       .b = -1,
                                       .imm = label});
  return b->nir.len != before ||
         ny_native_nir_fail(b, "native NYIR lower: allocation failed");
}

static bool ny_native_nir_emit_br(ny_native_nir_builder_t *b, int label) {
  size_t before = b->nir.len;
  ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NY_NIR_BR,
                                       .dst = -1,
                                       .a = -1,
                                       .b = -1,
                                       .imm = label});
  return b->nir.len != before ||
         ny_native_nir_fail(b, "native NYIR lower: allocation failed");
}

static bool ny_native_nir_emit_br_if(ny_native_nir_builder_t *b, int value,
                                     int label) {
  size_t before = b->nir.len;
  ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NY_NIR_BR_IF,
                                       .dst = -1,
                                       .a = value,
                                       .b = -1,
                                       .imm = label});
  return b->nir.len != before ||
         ny_native_nir_fail(b, "native NYIR lower: allocation failed");
}

static bool ny_native_nir_emit_ret(ny_native_nir_builder_t *b, int value) {
  size_t before = b->nir.len;
  ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NY_NIR_RET,
                                       .dst = -1,
                                       .a = value,
                                       .b = -1});
  if (b->nir.len == before)
    return ny_native_nir_fail(b, "native NYIR lower: allocation failed");
  b->emitted_return = true;
  b->last_value = value;
  return true;
}

static int ny_native_nir_emit_const(ny_native_nir_builder_t *b, int64_t value) {
  int v = ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NY_NIR_CONST_I64,
                                               .dst = -1,
                                               .a = -1,
                                               .b = -1,
                                               .imm = value});
  if (v < 0)
    ny_native_nir_fail(b, "native NYIR lower: allocation failed");
  return v;
}

static int ny_native_nir_emit_const_f64(ny_native_nir_builder_t *b, double value) {
  int v = ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NYIR_CONST_F64,
                                               .dst = -1,
                                               .a = -1,
                                               .b = -1,
                                               .imm = ny_native_f64_bits(value)});
  if (v < 0)
    ny_native_nir_fail(b, "native NYIR lower: allocation failed");
  return v;
}

static int ny_native_nir_emit_const_f32(ny_native_nir_builder_t *b, double value) {
  int v = ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NYIR_CONST_F32,
                                               .dst = -1,
                                               .a = -1,
                                               .b = -1,
                                               .imm = ny_native_f32_bits((float)value)});
  if (v < 0)
    ny_native_nir_fail(b, "native NYIR lower: allocation failed");
  return v;
}

static int ny_native_nir_emit_i64_to_f64(ny_native_nir_builder_t *b, int value) {
  int v = ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NYIR_I64_TO_F64,
                                               .dst = -1,
                                               .a = value,
                                               .b = -1});
  if (v < 0)
    ny_native_nir_fail(b, "native NYIR lower: allocation failed");
  return v;
}

static int ny_native_nir_emit_i64_to_f32(ny_native_nir_builder_t *b, int value) {
  int v = ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NYIR_I64_TO_F32,
                                               .dst = -1,
                                               .a = value,
                                               .b = -1});
  if (v < 0)
    ny_native_nir_fail(b, "native NYIR lower: allocation failed");
  return v;
}

static int ny_native_nir_emit_f32_to_f64(ny_native_nir_builder_t *b, int value) {
  int v = ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NYIR_F32_TO_F64,
                                               .dst = -1,
                                               .a = value,
                                               .b = -1});
  if (v < 0)
    ny_native_nir_fail(b, "native NYIR lower: allocation failed");
  return v;
}

static int ny_native_nir_emit_f64_to_f32(ny_native_nir_builder_t *b, int value) {
  int v = ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NYIR_F64_TO_F32,
                                               .dst = -1,
                                               .a = value,
                                               .b = -1});
  if (v < 0)
    ny_native_nir_fail(b, "native NYIR lower: allocation failed");
  return v;
}

static int ny_native_nir_emit_add_i64(ny_native_nir_builder_t *b, int a,
                                      int rhs) {
  int v = ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NY_NIR_ADD_I64,
                                               .dst = -1,
                                               .a = a,
                                               .b = rhs});
  if (v < 0)
    ny_native_nir_fail(b, "native NYIR lower: allocation failed");
  return v;
}

static int ny_native_nir_emit_load_i64(ny_native_nir_builder_t *b, int addr) {
  int v = ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NYIR_LOAD_I64,
                                               .dst = -1,
                                               .a = addr,
                                               .b = -1});
  if (v < 0)
    ny_native_nir_fail(b, "native NYIR lower: allocation failed");
  return v;
}

static int ny_native_nir_emit_addr_local(ny_native_nir_builder_t *b, int slot,
                                         const char *symbol) {
  int v = ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NYIR_ADDR_LOCAL,
                                               .dst = -1,
                                               .a = -1,
                                               .b = -1,
                                               .imm = slot,
                                               .symbol = symbol});
  if (v < 0)
    ny_native_nir_fail(b, "native NYIR lower: allocation failed");
  return v;
}

static bool ny_native_nir_emit_store_i64(ny_native_nir_builder_t *b, int addr,
                                         int value) {
  size_t before = b->nir.len;
  ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NYIR_STORE_I64,
                                       .dst = -1,
                                       .a = addr,
                                       .b = -1,
                                       .c = value});
  return b->nir.len != before ||
         ny_native_nir_fail(b, "native NYIR lower: allocation failed");
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
  case NY_E_BINARY:
    return ny_native_nir_expr_is_f64(b, e->as.binary.left) ||
           ny_native_nir_expr_is_f64(b, e->as.binary.right);
  case NY_E_UNARY:
    return ny_native_nir_expr_is_f64(b, e->as.unary.right);
  case NY_E_CALL:
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
  case NY_E_BINARY:
    return ny_native_nir_expr_is_f32(b, e->as.binary.left) ||
           ny_native_nir_expr_is_f32(b, e->as.binary.right);
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

static bool ny_native_nir_store_local_value(ny_native_nir_builder_t *b,
                                            int slot, int value) {
  size_t before = b->nir.len;
  ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NY_NIR_STORE_LOCAL,
                                       .dst = -1,
                                       .a = value,
                                       .b = -1,
                                       .imm = slot});
  return b->nir.len != before ||
         ny_native_nir_fail(b, "native NYIR lower: allocation failed");
}

static int ny_native_nir_load_local_value(ny_native_nir_builder_t *b,
                                          int slot) {
  int v = ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NY_NIR_LOAD_LOCAL,
                                               .dst = -1,
                                               .a = -1,
                                               .b = -1,
                                               .imm = slot});
  if (v < 0)
    ny_native_nir_fail(b, "native NYIR lower: allocation failed");
  return v;
}

static int ny_native_nir_emit_is_zero(ny_native_nir_builder_t *b, int value) {
  int zero = ny_native_nir_emit_const(b, 0);
  if (zero < 0)
    return -1;
  int v = ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NY_NIR_CMP_I64,
                                               .dst = -1,
                                               .a = value,
                                               .b = zero,
                                               .cmp = NY_NIR_CMP_EQ});
  if (v < 0)
    ny_native_nir_fail(b, "native NYIR lower: allocation failed");
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
    if (e->as.literal.kind != NY_LIT_INT) {
      ny_native_nir_fail(b, "native NYIR lower: only int/bool/f64/nil literals are supported");
      return -1;
    }
    return ny_native_nir_emit_const(b, e->as.literal.as.i);
  case NY_E_IDENT: {
    ny_native_nir_local_t *l = ny_native_nir_find_local(b, e->as.ident.name);
    if (!l) {
      int addr = ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NYIR_ADDR_SYMBOL,
                                                      .dst = -1,
                                                      .a = -1,
                                                      .b = -1,
                                                      .imm = 0,
                                                      .symbol = e->as.ident.name});
      if (addr < 0)
        ny_native_nir_fail(b, "native NYIR lower: allocation failed");
      return addr;
    }
    int v = ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NY_NIR_LOAD_LOCAL,
                                                 .dst = -1,
                                                 .a = -1,
                                                 .b = -1,
                                                 .imm = l->slot,
                                                 .symbol = l->name});
    if (v < 0)
      ny_native_nir_fail(b, "native NYIR lower: allocation failed");
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
      return ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NY_NIR_SUB_I64,
                                                  .dst = -1,
                                                  .a = zero,
                                                  .b = rv});
    }
    if (strcmp(e->as.unary.op, "!") == 0) {
      int zero = ny_native_nir_emit_const(b, 0);
      if (zero < 0)
        return -1;
      return ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NY_NIR_CMP_I64,
                                                  .dst = -1,
                                                  .a = rv,
                                                  .b = zero,
                                                  .cmp = NY_NIR_CMP_EQ});
    }
    if (strcmp(e->as.unary.op, "~") == 0) {
      int mask = ny_native_nir_emit_const(b, -1);
      if (mask < 0)
        return -1;
      return ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NY_NIR_XOR_I64,
                                                  .dst = -1,
                                                  .a = rv,
                                                  .b = mask});
    }
    ny_native_nir_fail(b, "native NYIR lower: unsupported unary operator '%s'",
                       e->as.unary.op);
    return -1;
  }
  case NY_E_BINARY: {
    ny_nir_op_t op = NY_NIR_NOP;
    ny_nir_cmp_t cmp = NY_NIR_CMP_EQ;
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
    int v = ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = use_f64_cmp ? NYIR_CMP_F64
                                                       : use_f32_cmp ? NYIR_CMP_F32
                                                       : is_cmp    ? NY_NIR_CMP_I64
                                                                   : op,
                                                 .dst = -1,
                                                 .a = a,
                                                 .b = rhs,
                                                 .cmp = cmp});
    if (v < 0)
      ny_native_nir_fail(b, "native NYIR lower: allocation failed");
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
    /* Keep the standalone native path independent of the stdlib implementation
     * of print. NYIR integer values are raw, while the runtime print primitive
     * consumes a tagged Nytrix integer, so tag exactly at this ABI boundary. */
    if (leaf && strcmp(leaf, "print") == 0 &&
        !ny_native_nir_find_user_function(b, name)) {
      if (e->as.call.args.len != 1 || e->as.call.args.data[0].name) {
        ny_native_nir_fail(
            b, "native NYIR lower: print currently requires one positional integer argument");
        return -1;
      }
      if (ny_native_nir_expr_is_f64(b, e->as.call.args.data[0].val) ||
          ny_native_nir_expr_is_f32(b, e->as.call.args.data[0].val)) {
        ny_native_nir_fail(
            b, "native NYIR lower: print currently supports integer arguments only");
        return -1;
      }
      int raw = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
      if (raw < 0)
        return -1;
      int tagged = ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NY_NIR_ADD_I64,
                                                        .dst = -1,
                                                        .a = raw,
                                                        .b = raw});
      if (tagged < 0)
        return ny_native_nir_fail(b, "native NYIR lower: allocation failed"), -1;
      if (ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NY_NIR_CALL,
                                               .dst = -1,
                                               .a = tagged,
                                               .b = -1,
                                               .c = -1,
                                               .imm = 1,
                                               .flags = NY_NIR_INST_F_EXTERN,
                                               .symbol = "rt_print_int"}) < 0 ||
          ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NY_NIR_CALL,
                                               .dst = -1,
                                               .a = -1,
                                               .b = -1,
                                               .c = -1,
                                               .imm = 0,
                                               .flags = NY_NIR_INST_F_EXTERN,
                                               .symbol = "rt_print_newline"}) < 0) {
        ny_native_nir_fail(b, "native NYIR lower: allocation failed");
        return -1;
      }
      return ny_native_nir_emit_const(b, 0);
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
        int v = ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NYIR_ADDR_SYMBOL,
                                                     .dst = -1,
                                                     .a = -1,
                                                     .b = -1,
                                                     .imm = 0,
                                                     .symbol = local_name});
        if (v < 0)
          ny_native_nir_fail(b, "native NYIR lower: allocation failed");
        return v;
      }
      return ny_native_nir_emit_addr_local(b, l->slot, local_name);
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
    if (e->as.call.args.len > NY_NIR_CALL_MAX_ARGS) {
      ny_native_nir_fail(b,
                         "native NYIR lower: call exceeds the maximum supported argument count (%d)",
                         NY_NIR_CALL_MAX_ARGS);
      return -1;
    }
    const stmt_t *callee_fn = ny_native_nir_find_user_function(b, name);
    int args[NY_NIR_CALL_MAX_ARGS];
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
        ext->ret_aggregate_classes[0] != NY_SYSV_AGG_SSE) {
      ny_native_nir_fail(
          b, "native NYIR lower: SysV aggregate return class is not represented yet");
      return -1;
    }
    if (ext) {
      for (unsigned i = 0;
           i < ext->param_count && i < e->as.call.args.len; ++i) {
        if (ext->arg_aggregate_sizes[i] > 0 &&
            NY_NIR_ARG_AGG_SIZE(ext->arg_aggregate_sizes[i]) <= 16 &&
            (NY_NIR_ARG_AGG_CLASS(ext->arg_aggregate_sizes[i], 0) ==
                 NY_SYSV_AGG_UNSUPPORTED ||
             NY_NIR_ARG_AGG_CLASS(ext->arg_aggregate_sizes[i], 0) ==
                 NY_SYSV_AGG_NONE)) {
          ny_native_nir_fail(
              b, "native NYIR lower: SysV register aggregate argument is not represented yet");
          return -1;
        }
      }
    }
    int aggregate_ret_ptr = -1;
    if (has_aggregate_return) {
      aggregate_ret_ptr = ny_nir_emit(
          &b->nir,
          (ny_nir_inst_t){.op = NYIR_ALLOCA,
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
          ny_native_nir_fail(b, "native NYIR lower: allocation failed");
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
      if (argc > NY_NIR_CALL_MAX_ARGS) {
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
    unsigned flags = (ext || builtin_c_call) ? NY_NIR_INST_F_EXTERN : 0;
    if (callee_fn && ny_native_type_name_is_f64(callee_fn->as.fn.return_type)) {
      flags |= NY_NIR_INST_F_RET_F64;
    } else if (callee_fn && ny_native_type_name_is_f32(callee_fn->as.fn.return_type)) {
      flags |= NY_NIR_INST_F_RET_F32;
    } else if (ny_native_nir_expr_is_f64(b, e)) {
      flags |= NY_NIR_INST_F_RET_F64;
    }
    if (has_aggregate_return && !has_sret &&
        ext->ret_aggregate_classes[0] == NY_SYSV_AGG_SSE)
      flags |= NY_NIR_INST_F_RET_F64;
    int *extra = NULL;
    if (argc > 6) {
      size_t extra_len = argc - 6;
      extra = (int *)malloc(extra_len * sizeof(*extra));
      if (!extra) {
        free(arg_sizes);
        ny_native_nir_fail(b, "native NYIR lower: allocation failed");
        return -1;
      }
      memcpy(extra, &args[6], extra_len * sizeof(*extra));
    }
    int v = ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NY_NIR_CALL,
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
      ny_native_nir_fail(b, "native NYIR lower: allocation failed");
      return -1;
    }
    int primary_ret = v;
    int second_ret = -1;
    bool capture_second_integer_first =
        has_aggregate_return && !has_sret &&
        ext->ret_aggregate_classes[0] == NY_SYSV_AGG_SSE &&
        ext->ret_aggregate_classes[1] == NY_SYSV_AGG_INTEGER;
    if (capture_second_integer_first) {
      second_ret = ny_nir_emit(
          &b->nir, (ny_nir_inst_t){.op = NYIR_CAPTURE_RET,
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
      primary_ret = ny_nir_emit(
          &b->nir, (ny_nir_inst_t){.op = NYIR_CAPTURE_RET,
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
            b, "native NYIR lower: secondary SysV aggregate return class is not represented yet");
        return -1;
      }
      second_ret = ny_nir_emit(
          &b->nir, (ny_nir_inst_t){.op = NYIR_CAPTURE_RET,
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
    int v = ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NYIR_LOAD_I64,
                                                 .dst = -1,
                                                 .a = addr,
                                                 .b = -1,
                                                 .c = -1});
    if (v < 0)
      ny_native_nir_fail(b, "native NYIR lower: deref load failed");
    return v;
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
  int else_label = b->next_label++;
  int end_label = b->next_label++;

  int cond_val = ny_native_nir_lower_expr(b, cond);
  if (cond_val < 0)
    return -1;
  int cond_zero = ny_native_nir_emit_is_zero(b, cond_val);
  if (cond_zero < 0 || !ny_native_nir_emit_br_if(b, cond_zero, else_label))
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
    size_t before = b->nir.len;
    ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NY_NIR_STORE_LOCAL,
                                         .dst = -1,
                                         .a = val,
                                         .b = -1,
                                         .imm = l->slot,
                                         .symbol = l->name});
    if (b->nir.len == before)
      return ny_native_nir_fail(b, "native NYIR lower: allocation failed");
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
  int zero = ny_native_nir_emit_const(b, 0);
  if (zero < 0)
    return false;
  int is_false = ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NY_NIR_CMP_I64,
                                                       .dst = -1,
                                                       .a = cond,
                                                       .b = zero,
                                                       .cmp = NY_NIR_CMP_EQ});
  if (is_false < 0)
    return ny_native_nir_fail(b, "native NYIR lower: allocation failed");
  int else_label = b->next_label++;
  int merge_label = b->next_label++;
  int end_label = b->next_label++;
  if (!ny_native_nir_emit_br_if(b, is_false, else_label))
    return false;

  int result_slot = ny_native_nir_temp_slot(b);
  bool has_alt = s->as.iff.alt != NULL;
  bool entry_return = b->emitted_return;
  int entry_last_value = b->last_value;

  /* If no else, pre-store entry_last_value as the false-branch result. */
  if (!has_alt && !entry_return && entry_last_value >= 0) {
    size_t before = b->nir.len;
    ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NY_NIR_STORE_LOCAL,
                                         .dst = -1,
                                         .a = entry_last_value,
                                         .b = -1,
                                         .imm = result_slot});
    if (b->nir.len == before)
      return ny_native_nir_fail(b, "native NYIR lower: allocation failed");
  }

  /* Then branch. */
  b->emitted_return = false;
  if (!ny_native_nir_lower_stmt(b, s->as.iff.conseq))
    return false;
  bool conseq_returns = b->emitted_return;
  if (!conseq_returns) {
    int conseq_val = b->last_value;
    if (conseq_val >= 0) {
      size_t before = b->nir.len;
      ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NY_NIR_STORE_LOCAL,
                                           .dst = -1,
                                           .a = conseq_val,
                                           .b = -1,
                                           .imm = result_slot});
      if (b->nir.len == before)
        return ny_native_nir_fail(b, "native NYIR lower: allocation failed");
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
      size_t before = b->nir.len;
      ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NY_NIR_STORE_LOCAL,
                                           .dst = -1,
                                           .a = alt_val,
                                           .b = -1,
                                           .imm = result_slot});
      if (b->nir.len == before)
        return ny_native_nir_fail(b, "native NYIR lower: allocation failed");
    }
  }

  /* Merge point: both branches converge here. */
  b->emitted_return = entry_return || (has_alt && conseq_returns && alt_returns);
  if (!b->emitted_return) {
    if (!ny_native_nir_emit_label(b, merge_label))
      return false;
    int loaded = ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NY_NIR_LOAD_LOCAL,
                                                       .dst = -1,
                                                       .a = -1,
                                                       .b = -1,
                                                       .imm = result_slot});
    if (loaded < 0)
      return ny_native_nir_fail(b, "native NYIR lower: allocation failed");
    b->last_value = loaded;
  }
  return ny_native_nir_emit_label(b, end_label);
}

static bool ny_native_nir_lower_while(ny_native_nir_builder_t *b, const stmt_t *s) {
  if (s->as.whl.init && !ny_native_nir_lower_stmt(b, s->as.whl.init))
    return false;
  int head_label = b->next_label++;
  int update_label = s->as.whl.update ? b->next_label++ : head_label;
  int end_label = b->next_label++;
  if (!ny_native_nir_emit_label(b, head_label))
    return false;
  int cond = ny_native_nir_lower_expr(b, s->as.whl.test);
  if (cond < 0)
    return false;
  int zero = ny_native_nir_emit_const(b, 0);
  if (zero < 0)
    return false;
  int is_false = ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NY_NIR_CMP_I64,
                                                      .dst = -1,
                                                      .a = cond,
                                                      .b = zero,
                                                      .cmp = NY_NIR_CMP_EQ});
  if (is_false < 0)
    return ny_native_nir_fail(b, "native NYIR lower: allocation failed");
  if (!ny_native_nir_emit_br_if(b, is_false, end_label))
    return false;
  bool entry_return = b->emitted_return;
  int entry_last_value = b->last_value;
  if (b->loop_depth >= sizeof(b->loop_head_labels) / sizeof(b->loop_head_labels[0]))
    return ny_native_nir_fail(b, "native NYIR lower: loop nesting limit exceeded");
  size_t loop_i = b->loop_depth++;
  b->loop_head_labels[loop_i] = head_label;
  b->loop_continue_labels[loop_i] = update_label;
  b->loop_end_labels[loop_i] = end_label;
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


static bool ny_native_nir_lower_for_header(ny_native_nir_builder_t *b,
                                           const stmt_t *s) {
  if (s->as.fr.init && !ny_native_nir_lower_stmt(b, s->as.fr.init))
    return false;
  int head_label = b->next_label++;
  int update_label = s->as.fr.update ? b->next_label++ : head_label;
  int end_label = b->next_label++;
  if (!ny_native_nir_emit_label(b, head_label))
    return false;
  int cond = s->as.fr.cond ? ny_native_nir_lower_expr(b, s->as.fr.cond)
                           : ny_native_nir_emit_const(b, 1);
  if (cond < 0)
    return false;
  int is_false = ny_native_nir_emit_is_zero(b, cond);
  if (is_false < 0 || !ny_native_nir_emit_br_if(b, is_false, end_label))
    return false;
  if (b->loop_depth >= sizeof(b->loop_head_labels) / sizeof(b->loop_head_labels[0]))
    return ny_native_nir_fail(b, "native NYIR lower: loop nesting limit exceeded");
  size_t loop_i = b->loop_depth++;
  b->loop_head_labels[loop_i] = head_label;
  b->loop_continue_labels[loop_i] = update_label;
  b->loop_end_labels[loop_i] = end_label;
  bool entry_return = b->emitted_return;
  int entry_last_value = b->last_value;
  b->emitted_return = false;
  bool body_ok = ny_native_nir_lower_stmt(b, s->as.fr.body);
  if (body_ok && s->as.fr.update) {
    b->emitted_return = false;
    body_ok = ny_native_nir_emit_label(b, update_label) &&
              ny_native_nir_lower_stmt(b, s->as.fr.update);
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
  int end_label = b->next_label++;
  if (!ny_native_nir_emit_label(b, head_label))
    return false;

  int cur = ny_native_nir_load_local_value(b, iter->slot);
  int end = ny_native_nir_load_local_value(b, hi_slot);
  int in_range = ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NY_NIR_CMP_I64,
                                                      .dst = -1,
                                                      .a = cur,
                                                      .b = end,
                                                      .cmp = NY_NIR_CMP_LE});
  int done = in_range >= 0 ? ny_native_nir_emit_is_zero(b, in_range) : -1;
  if (done < 0 || !ny_native_nir_emit_br_if(b, done, end_label))
    return false;

  if (b->loop_depth >= sizeof(b->loop_head_labels) / sizeof(b->loop_head_labels[0]))
    return ny_native_nir_fail(b, "native NYIR lower: loop nesting limit exceeded");
  size_t loop_i = b->loop_depth++;
  b->loop_head_labels[loop_i] = head_label;
  b->loop_continue_labels[loop_i] = update_label;
  b->loop_end_labels[loop_i] = end_label;
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
                 ? ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NY_NIR_ADD_I64,
                                                         .dst = -1,
                                                         .a = cur,
                                                         .b = one})
                 : -1;
  if (next < 0 || !ny_native_nir_store_local_value(b, iter->slot, next))
    return false;
  if (index) {
    int old_idx = ny_native_nir_load_local_value(b, index->slot);
    int next_idx = old_idx >= 0
                       ? ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NY_NIR_ADD_I64,
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
                                  ny_nir_cmp_t cmp) {
  int v = ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NY_NIR_CMP_I64,
                                               .dst = -1,
                                               .a = a,
                                               .b = rhs,
                                               .cmp = cmp});
  if (v < 0)
    ny_native_nir_fail(b, "native NYIR lower: allocation failed");
  return v;
}

static int ny_native_nir_emit_bool_binop(ny_native_nir_builder_t *b,
                                         ny_nir_op_t op, int a, int rhs) {
  int v = ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = op,
                                               .dst = -1,
                                               .a = a,
                                               .b = rhs});
  if (v < 0)
    ny_native_nir_fail(b, "native NYIR lower: allocation failed");
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
    int ge = ny_native_nir_emit_cmp(b, test_value, lo, NY_NIR_CMP_GE);
    int le = ny_native_nir_emit_cmp(b, test_value, hi, NY_NIR_CMP_LE);
    if (ge < 0 || le < 0)
      return -1;
    return ny_native_nir_emit_bool_binop(b, NY_NIR_AND_I64, ge, le);
  }

  int rhs = ny_native_nir_lower_expr(b, pat);
  if (rhs < 0)
    return -1;
  return ny_native_nir_emit_cmp(b, test_value, rhs, NY_NIR_CMP_EQ);
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
                   : ny_native_nir_emit_bool_binop(b, NY_NIR_OR_I64,
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
    return ny_native_nir_emit_br(b, b->loop_end_labels[b->loop_depth - 1]);
  case NY_S_CONTINUE:
    if (b->loop_depth == 0)
      return ny_native_nir_fail(b, "native NYIR lower: continue outside loop");
    b->emitted_return = true;
    return ny_native_nir_emit_br(b, b->loop_continue_labels[b->loop_depth - 1]);
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
  ny_nir_opt_stats_t stats;
  if (!ny_nir_optimize_with_stats(&b->nir, &stats)) {
    if (verbose_enabled >= 1)
      fprintf(stderr, "nyir opt FAILED\n");
    if (err && err_len > 0 && err[0] == '\0')
      snprintf(err, err_len, "native NYIR: optimization failed");
    goto fail;
  }
  if (!ny_nir_verify(&b->nir, err, err_len))
    goto fail;
  if (verbose_enabled >= 1 && stats.pass_time_ms[8] > 0.001) {
    size_t removed = stats.before_insts - stats.after_insts;
    double pct = stats.before_insts > 0
                     ? 100.0 * (double)removed / stats.before_insts
                     : 0.0;
    fprintf(stderr, "nyir finalize: %zu→%zu insts (-%zu, %.1f%%) in %.2fms\n",
            stats.before_insts, stats.after_insts, removed, pct,
            stats.pass_time_ms[8]);
  }
  return true;
fail:
  /* Reduced repro dump on failure. */
  fprintf(stderr, "native NYIR repro (optimize/verify failed):\n");
  ny_nir_dump(stderr, &b->nir, "<failed>");
  return false;
}

static bool ny_native_nir_opt_dump(FILE *out, ny_native_nir_builder_t *b,
                                   const char *name, const ny_options *opt) {
  if (opt && opt->nyir_dump_raw) {
    fprintf(out, "nyir raw %s\n", name && name[0] ? name : "<anon>");
    ny_nir_dump(out, &b->nir, name);
  }
  ny_nir_opt_stats_t stats;
  if (!ny_nir_optimize_with_stats(&b->nir, &stats) ||
      !ny_nir_verify(&b->nir, b->err, b->err_len)) {
    if (b->err && b->err_len > 0 && b->err[0] == '\0')
      ny_native_set_err(b->err, b->err_len, "native NYIR dump: optimization failed");
    return false;
  }
  if (opt && opt->nyir_dump_stats)
    ny_nir_dump_stats(out, &stats);
  ny_nir_dump(out, &b->nir, name);
  return true;
}

static bool ny_native_nir_opt_dump_binary(FILE *out, ny_native_nir_builder_t *b,
                                          const char *name, char *err,
                                          size_t err_len) {
  ny_nir_opt_stats_t stats;
  if (!ny_nir_optimize_with_stats(&b->nir, &stats) ||
      !ny_nir_verify(&b->nir, b->err, b->err_len)) {
    if (err && err_len > 0 && err[0] == '\0')
      ny_native_set_err(err, err_len, "native NYIR binary dump: optimization failed");
    return false;
  }
  if (!ny_nir_dump_binary(out, &b->nir, name)) {
    ny_native_set_err(err, err_len, "native NYIR binary dump: write failed");
    return false;
  }
  return true;
}

/*
 * Lower a single function stmt into a finalized ny_nir_func_t.
 * Returns true on success; caller must ny_nir_func_free(out) when done.
 */
static bool ny_native_nir_build_function(const program_t *prog, const stmt_t *fn,
                                        ny_nir_func_t *out, char *err,
                                        size_t err_len) {
  if (!fn || fn->kind != NY_S_FUNC || !out)
    return false;
  memset(out, 0, sizeof(*out));
  ny_native_nir_builder_t b = {.last_value = -1, .err = err, .err_len = err_len,
                               .prog = prog};
  for (size_t i = 0; i < fn->as.fn.params.len; ++i) {
    if (!ny_native_nir_bind_local_typed(
            &b, fn->as.fn.params.data[i].name,
            ny_native_type_name_is_f64(fn->as.fn.params.data[i].type),
            ny_native_type_name_is_f32(fn->as.fn.params.data[i].type))) {
      ny_nir_func_free(&b.nir);
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
    *out = b.nir;
  else
    ny_nir_func_free(&b.nir);
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
static bool ny_native_nir_collect_extern(const stmt_t *s, ny_extern_table_t *t,
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
          !ny_native_sysv_classify_aggregate(&parser, &decl.type,
                                             ret_agg_classes))
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
          if (arg_agg[pi] > NY_NIR_ARG_AGG_SIZE_MASK ||
              !ny_native_sysv_classify_aggregate(&parser, pt, classes)) {
            classes[0] = NY_SYSV_AGG_UNSUPPORTED;
            classes[1] = NY_SYSV_AGG_NONE;
          }
          arg_agg[pi] =
              (arg_agg[pi] & NY_NIR_ARG_AGG_SIZE_MASK) |
              ((uint32_t)classes[0] << NY_NIR_ARG_AGG_CLASS0_SHIFT) |
              ((uint32_t)classes[1] << NY_NIR_ARG_AGG_CLASS1_SHIFT);
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
    free(src);
    return true;
  }
  /* Recurse through container statements, mirroring process_links(). */
  if (s->kind == NY_S_MODULE) {
    for (size_t i = 0; i < s->as.module.body.len; ++i)
      if (!ny_native_nir_collect_extern(s->as.module.body.data[i], t, err, err_len))
        return false;
    return true;
  }
  if (s->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      if (!ny_native_nir_collect_extern(s->as.block.body.data[i], t, err, err_len))
        return false;
    return true;
  }
  if (s->kind == NY_S_IF) {
    if (s->as.iff.conseq &&
        !ny_native_nir_collect_extern(s->as.iff.conseq, t, err, err_len))
      return false;
    if (s->as.iff.alt &&
        !ny_native_nir_collect_extern(s->as.iff.alt, t, err, err_len))
      return false;
    return true;
  }
  if (s->kind == NY_S_WHILE) {
    if (s->as.whl.body &&
        !ny_native_nir_collect_extern(s->as.whl.body, t, err, err_len))
      return false;
    if (s->as.whl.update &&
        !ny_native_nir_collect_extern(s->as.whl.update, t, err, err_len))
      return false;
    if (s->as.whl.init &&
        !ny_native_nir_collect_extern(s->as.whl.init, t, err, err_len))
      return false;
    return true;
  }
  if (s->kind == NY_S_FOR) {
    if (s->as.fr.init &&
        !ny_native_nir_collect_extern(s->as.fr.init, t, err, err_len))
      return false;
    if (s->as.fr.body &&
        !ny_native_nir_collect_extern(s->as.fr.body, t, err, err_len))
      return false;
    if (s->as.fr.update &&
        !ny_native_nir_collect_extern(s->as.fr.update, t, err, err_len))
      return false;
    return true;
  }
  if (s->kind == NY_S_TRY) {
    if (s->as.tr.body &&
        !ny_native_nir_collect_extern(s->as.tr.body, t, err, err_len))
      return false;
    if (s->as.tr.handler &&
        !ny_native_nir_collect_extern(s->as.tr.handler, t, err, err_len))
      return false;
    return true;
  }
  if (s->kind == NY_S_DEFER) {
    if (s->as.de.body &&
        !ny_native_nir_collect_extern(s->as.de.body, t, err, err_len))
      return false;
    return true;
  }
  if (s->kind == NY_S_MATCH) {
    for (size_t i = 0; i < s->as.match.arms.len; ++i)
      if (s->as.match.arms.data[i].conseq &&
          !ny_native_nir_collect_extern(s->as.match.arms.data[i].conseq, t, err,
                                        err_len))
        return false;
    if (s->as.match.default_conseq &&
        !ny_native_nir_collect_extern(s->as.match.default_conseq, t, err, err_len))
      return false;
    return true;
  }
  return true;
}

static bool ny_native_nir_build_extern_table(const program_t *prog,
                                              ny_extern_table_t *t,
                                              char *err, size_t err_len) {
  if (!t)
    return false;
  ny_extern_table_init(t);
  if (!prog)
    return true;
  for (size_t i = 0; i < prog->body.len; ++i) {
    const stmt_t *s = prog->body.data[i];
    if (!ny_native_nir_collect_extern(s, t, err, err_len))
      return false;
  }
  return true;
}

/*
 * Lower the top-level program statements into a finalized ny_nir_func_t for
 * rt_main.  Returns true on success; caller must ny_nir_func_free(out).
 */
static bool ny_native_nir_build_rt_main(const program_t *prog, ny_nir_func_t *out,
                                        const ny_extern_table_t *externs,
                                        char *err, size_t err_len) {
  if (!out)
    return false;
  memset(out, 0, sizeof(*out));
  ny_native_nir_builder_t b = {.last_value = -1, .err = err, .err_len = err_len,
                               .externs = externs, .prog = prog};
  for (size_t i = 0; prog && i < prog->body.len; ++i) {
    if (!ny_native_nir_lower_stmt(&b, prog->body.data[i])) {
      ny_nir_func_free(&b.nir);
      return false;
    }
    if (b.emitted_return)
      break;
  }
  if (!b.emitted_return) {
    if (b.last_value < 0) {
      ny_native_nir_fail(&b, "native NYIR: program has no raw expression result");
      ny_nir_func_free(&b.nir);
      return false;
    }
    if (!ny_native_nir_emit_ret(&b, b.last_value)) {
      ny_nir_func_free(&b.nir);
      return false;
    }
  }
  bool ok = ny_native_nir_finalize(&b, err, err_len);
  if (ok)
    *out = b.nir;
  else
    ny_nir_func_free(&b.nir);
  return ok;
}

bool ny_native_build_nir(const program_t *prog, const ny_options *opt,
                         ny_nir_func_t *rt_main_out,
                         ny_nir_func_t *funcs_out, size_t *func_count,
                         size_t max_funcs, char *err, size_t err_len) {
  if (!prog || !rt_main_out)
    return false;
  (void)opt; /* reserved for future NYIR pass-level options */
  memset(rt_main_out, 0, sizeof(*rt_main_out));
  if (func_count)
    *func_count = 0;

  /* Build extern table from #include and extern statements. */
  ny_extern_table_t externs;
  char extern_err[256] = {0};
  if (!ny_native_nir_build_extern_table(prog, &externs, extern_err,
                                        sizeof(extern_err))) {
    ny_native_set_err(err, err_len, "NYIR extern: %s", extern_err);
    ny_extern_table_free(&externs);
    return false;
  }

  /* Build user functions first. */
  if (funcs_out && func_count && max_funcs > 0) {
    size_t count = 0;
    for (size_t i = 0; i < prog->body.len && count < max_funcs; ++i) {
      const stmt_t *s = prog->body.data[i];
      if (!s || s->kind != NY_S_FUNC)
        continue;
      char local_err[256] = {0};
      if (!ny_native_nir_build_function(prog, s, &funcs_out[count], local_err,
                                       sizeof(local_err))) {
        /* If lowering failed, free any already-built functions. */
        for (size_t j = 0; j < count; ++j)
          ny_nir_func_free(&funcs_out[j]);
        *func_count = 0;
        /* Non-fatal: function NYIR build failure is not an error for
         * diagnostics-only mode.  Fall through to rt_main. */
        if (err && err_len > 0 && local_err[0])
          ny_native_set_err(err, err_len, "%s", local_err);
      } else {
        count++;
      }
    }
    *func_count = count;
  }

  /* Build rt_main with extern table. */
  bool ok = ny_native_nir_build_rt_main(prog, rt_main_out, &externs, err, err_len);
  ny_extern_table_free(&externs);
  return ok;
}

bool ny_native_nir_dump_function(FILE *out, const stmt_t *fn, char *err,
                                 size_t err_len, const ny_options *opt) {
  if (!fn || fn->kind != NY_S_FUNC)
    return true;
  ny_native_nir_builder_t b = {.last_value = -1, .err = err, .err_len = err_len};
  for (size_t i = 0; i < fn->as.fn.params.len; ++i) {
    if (!ny_native_nir_bind_local_typed(
            &b, fn->as.fn.params.data[i].name,
            ny_native_type_name_is_f64(fn->as.fn.params.data[i].type),
            ny_native_type_name_is_f32(fn->as.fn.params.data[i].type))) {
      ny_nir_func_free(&b.nir);
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
  ny_nir_func_free(&b.nir);
  return ok;
}

bool ny_native_nir_dump_rt_main(FILE *out, const program_t *prog, char *err,
                                size_t err_len, const ny_options *opt) {
  ny_extern_table_t externs;
  if (!ny_native_nir_build_extern_table(prog, &externs, err, err_len)) {
    ny_extern_table_free(&externs);
    return false;
  }
  ny_native_nir_builder_t b = {.last_value = -1,
                               .err = err,
                               .err_len = err_len,
                               .externs = &externs,
                               .prog = prog};
  for (size_t i = 0; prog && i < prog->body.len; ++i) {
    if (!ny_native_nir_lower_stmt(&b, prog->body.data[i])) {
      ny_nir_func_free(&b.nir);
      ny_extern_table_free(&externs);
      return false;
    }
    if (b.emitted_return)
      break;
  }
  if (!b.emitted_return) {
    if (b.last_value < 0) {
      ny_native_nir_fail(&b, "native NYIR dump unavailable: program has no raw expression result");
      ny_nir_func_free(&b.nir);
      ny_extern_table_free(&externs);
      return false;
    }
    if (!ny_native_nir_emit_ret(&b, b.last_value)) {
      ny_nir_func_free(&b.nir);
      ny_extern_table_free(&externs);
      return false;
    }
  }
  bool ok = ny_native_nir_opt_dump(out, &b, "rt_main", opt);
  ny_nir_func_free(&b.nir);
  ny_extern_table_free(&externs);
  return ok;
}

size_t ny_native_nir_local_count(const ny_nir_func_t *f);
bool ny_native_ensure_parent_dir_for_path(const char *path);

bool ny_native_nir_dump_rt_main_binary(FILE *out, const program_t *prog,
                                       char *err, size_t err_len) {
  ny_extern_table_t externs;
  if (!ny_native_nir_build_extern_table(prog, &externs, err, err_len)) {
    ny_extern_table_free(&externs);
    return false;
  }
  ny_native_nir_builder_t b = {.last_value = -1,
                               .err = err,
                               .err_len = err_len,
                               .externs = &externs,
                               .prog = prog};
  for (size_t i = 0; prog && i < prog->body.len; ++i) {
    if (!ny_native_nir_lower_stmt(&b, prog->body.data[i])) {
      ny_nir_func_free(&b.nir);
      ny_extern_table_free(&externs);
      return false;
    }
    if (b.emitted_return)
      break;
  }
  if (!b.emitted_return) {
    if (b.last_value < 0) {
      ny_native_nir_fail(&b, "native NYIR binary dump unavailable: program has no raw expression result");
      ny_nir_func_free(&b.nir);
      ny_extern_table_free(&externs);
      return false;
    }
    if (!ny_native_nir_emit_ret(&b, b.last_value)) {
      ny_nir_func_free(&b.nir);
      ny_extern_table_free(&externs);
      return false;
    }
  }
  bool ok = ny_native_nir_opt_dump_binary(out, &b, "rt_main", err, err_len);
  ny_nir_func_free(&b.nir);
  ny_extern_table_free(&externs);
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
    ny_nir_func_t f = {0};
    char name[128] = {0};
    char local_err[512] = {0};
    bool ok = ny_nir_load_binary(in, &f, name, sizeof(name), local_err,
                                 sizeof(local_err));
    fclose(in);
    if (ok) {
      ny_nir_metadata_summary_t summary = {0};
      ok = ny_nir_metadata_summary(&f, &summary, local_err,
                                   sizeof(local_err));
      if (ok) {
        fprintf(out, "nyir metadata report functions=1 source=binary path=%s\n",
                opt->nyir_metadata_bin_path);
        ny_nir_metadata_summary_dump(out, name[0] ? name : "rt_main",
                                     &summary);
      }
    }
    ny_nir_func_free(&f);
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

  ny_nir_func_t rt_main = {0};
  ny_nir_func_t funcs[64] = {{0}};
  const char *names[64] = {0};
  size_t wanted_names = 0;
  for (size_t i = 0; prog && i < prog->body.len && wanted_names < 64; ++i) {
    const stmt_t *s = prog->body.data[i];
    if (s && s->kind == NY_S_FUNC)
      names[wanted_names++] = s->as.fn.name ? s->as.fn.name : "<fn>";
  }

  size_t func_count = 0;
  char local_err[512] = {0};
  bool ok = ny_native_build_nir(prog, opt, &rt_main, funcs, &func_count, 64,
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
    ny_nir_metadata_summary_t summary = {0};
    if (!ny_nir_metadata_summary(&funcs[i], &summary, local_err,
                                 sizeof(local_err))) {
      ok = false;
      break;
    }
    ny_nir_metadata_summary_dump(out, i < wanted_names ? names[i] : "<fn>",
                                 &summary);
  }
  if (ok) {
    ny_nir_metadata_summary_t summary = {0};
    if (ny_nir_metadata_summary(&rt_main, &summary, local_err,
                                sizeof(local_err)))
      ny_nir_metadata_summary_dump(out, "rt_main", &summary);
    else
      ok = false;
  }

  ny_nir_func_free(&rt_main);
  for (size_t i = 0; i < func_count; ++i)
    ny_nir_func_free(&funcs[i]);
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
